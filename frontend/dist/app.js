'use strict';

(() => {
  const $ = (id) => document.getElementById(id);
  const icon = (name) => `<svg class="icon" aria-hidden="true"><use href="#i-${name}"/></svg>`;
  const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const initialAnswer = '我可以回答问题，也可以描述摄像头画面。试试输入问题，或点击「描述当前画面」。';
  const exampleQuestion = '你好，介绍一下你能帮我做什么吧。';
  const describeQuestion = '描述一下你看到的画面';
  const localService = /^https?:$/.test(window.location.protocol) ? window.location.origin : '';
  const replyAudio = $('reply-audio');
  const ringGestureToggle = $('ring-gesture-toggle');
  const interactiveTarget = 'input, textarea, select, button, a[href], area[href], summary, audio[controls], video[controls], [contenteditable]:not([contenteditable="false"]), [tabindex]:not(#workspace-main, #answer-scroll, #ring-title, #ring-events-title)';
  const storage = {
    get(key, fallback) { try { return JSON.parse(localStorage.getItem(key)) ?? fallback; } catch { return fallback; } },
    set(key, value) { try { localStorage.setItem(key, JSON.stringify(value)); return true; } catch { return false; } }
  };
  const savedConfig = storage.get('connect-studio-config', {});
  let config = { mode: ['demo', 'browser', 'service'].includes(savedConfig?.mode) ? savedConfig.mode : 'demo', serviceUrl: typeof savedConfig?.serviceUrl === 'string' ? savedConfig.serviceUrl : '', videoUrl: typeof savedConfig?.videoUrl === 'string' ? savedConfig.videoUrl : '' };
  const rawHistory = storage.get('connect-studio-history', []);
  let history = Array.isArray(rawHistory) ? rawHistory.filter((item) => item && typeof item.id === 'string' && typeof item.text === 'string' && typeof item.answer === 'string' && typeof item.time === 'number').slice(0, 30) : [];
  const uid = () => globalThis.crypto?.randomUUID?.() || `${Date.now()}-${Math.random().toString(36).slice(2)}`;
  let currentId = uid(), answer = initialAnswer, answerQuestion = exampleQuestion, answerKind = 'demo', generation = 0, busy = false;
  let recording = false, recordStarted = 0, recordTimer = null, recognition = null, eventSource = null;
  let serviceStatus = null, requestController = null, serviceCapture = null;
  let speechActive = false, speechVersion = 0, answerAudioUrl = '', cameraVersion = 0;
  let serverPlayback = false, serverPlaybackTimer;
  let descriptionInProgress = false, pendingPlaybackStop = null;
  let ringGestureArmed = Boolean(localService) && ['127.0.0.1', 'localhost'].includes(window.location.hostname);
  let cameraStream = null, cameraKind = '', videoObjectUrl = '', audioObjectUrl = '', cameraTimeout = null, cameraPending = false;
  let toastTimer;

  function toast(message) {
    clearTimeout(toastTimer); $('toast').textContent = message; $('toast').hidden = false;
    toastTimer = setTimeout(() => { $('toast').hidden = true; }, 4700);
  }
  function displayTime(ms) { const seconds = Math.max(0, Math.floor(ms / 1000)); return `${String(Math.floor(seconds / 60)).padStart(2, '0')}:${String(seconds % 60).padStart(2, '0')}`; }
  function setTranscript(text, label = '识别完成') {
    $('transcript').value = text; $('transcript-badge').textContent = label; updateCount();
  }
  function updateCount() { $('character-count').textContent = `${Array.from($('transcript').value).length} 字`; }
  function setFlow(step) { ['listen', 'read', 'answer', 'speak'].forEach((name) => $(`flow-${name}`).classList.toggle('active', step === name)); }
  function selectMobilePanel(panel, focusAnswer = false) {
    const view = $('conversation-view');
    view.dataset.mobilePanel = panel;
    $('mobile-conversation-input').setAttribute('aria-pressed', String(panel === 'input'));
    $('mobile-conversation-answer').setAttribute('aria-pressed', String(panel === 'answer'));
    if (focusAnswer && !view.hidden && $('mobile-conversation-answer').getClientRects().length) {
      $('answer-scroll').focus({ preventScroll: true });
    }
  }
  function setBusy(value) {
    busy = value;
    ['send-button', 'upload-button', 'describe-button'].forEach((id) => { $(id).disabled = value; });
    $('record-button').disabled = value && !recording;
    $('answer-content').setAttribute('aria-busy', String(value && !recording));
    $('transcript').readOnly = value;
    $('speak-answer').disabled = value || !answer;
  }
  function setRecording(value) {
    recording = value; $('waveform').classList.toggle('active', value);
    $('record-button').classList.toggle('recording', value);
    $('record-button').innerHTML = `${icon(value ? 'stop' : 'mic')}<span>${value ? '结束语音输入' : '开始语音输入'}</span>`;
    clearInterval(recordTimer);
    if (value) {
      recordStarted = Date.now(); $('recording-time').textContent = '00:00'; setFlow('listen');
      recordTimer = setInterval(() => {
        const elapsed = Date.now() - recordStarted;
        $('recording-time').textContent = displayTime(elapsed);
        if (serviceCapture?.local && elapsed >= 12000) endRecording();
      }, 250);
    }
  }
  function clearDescriptionMedia() {
    answerAudioUrl = '';
    $('description-audio-status').hidden = true;
    replyAudio.removeAttribute('src');
    replyAudio.load();
    const image = $('description-image');
    image.onload = null;
    image.onerror = null;
    image.removeAttribute('src');
    $('description-photo').hidden = true;
  }
  function showAnswer(text, kind = 'demo', modelName = '') {
    clearDescriptionMedia();
    answer = text; answerKind = kind;
    $('answer-content').className = 'answer-content plain-answer'; $('answer-content').textContent = text;
    $('answer-scroll').scrollTop = 0;
    $('answer-badge').textContent = kind === 'demo' ? '演示回答' : '模型回答';
    $('answer-badge').className = 'badge green';
    $('answer-model').replaceChildren();
    const mark = document.createElement('span'); mark.className = 'model-mark'; mark.textContent = '✳';
    $('answer-model').append(mark, document.createTextNode(modelName || (kind === 'demo' ? '演示助手' : '已连接模型')));
    $('copy-answer').disabled = !text; $('speak-answer').disabled = !text;
  }
  function emptyAnswer(message = '输入问题后，回答会显示在这里。') {
    clearDescriptionMedia();
    answer = ''; answerQuestion = ''; $('answer-content').className = 'answer-content empty-answer';
    answerKind = config.mode === 'demo' ? 'demo' : 'model';
    $('answer-model').textContent = config.mode === 'demo' ? '✳ 演示助手' : (serviceStatus?.model?.connected ? (serviceStatus.model.name || '已连接模型') : '等待模型连接');
    $('answer-content').innerHTML = icon('spark'); const p = document.createElement('p'); p.textContent = message; $('answer-content').append(p);
    $('answer-scroll').scrollTop = 0;
    $('answer-badge').textContent = '等待提问'; $('answer-badge').className = 'badge neutral';
    $('copy-answer').disabled = true; $('speak-answer').disabled = true;
  }
  function normalizeUrl(value, required = false) {
    if (!value.trim()) { if (required) throw new Error('请先填写电脑端服务地址。'); return ''; }
    let url;
    try { url = new URL(value.trim()); } catch { throw new Error('地址格式不正确，请使用 http:// 或 https:// 开头的完整地址。'); }
    if (!['http:', 'https:'].includes(url.protocol) || url.username || url.password || url.hash) throw new Error('请使用不含账户信息或锚点的 HTTP / HTTPS 地址。');
    return url.href.replace(/\/$/, '');
  }
  function endpoint(path, base = config.serviceUrl || localService) { const url = normalizeUrl(base, true); return `${url.replace(/\/$/, '')}/${path.replace(/^\//, '')}`; }
  async function api(path, options = {}, base = config.serviceUrl || localService) {
    const controller = new AbortController(), signal = options.signal;
    const cancel = () => controller.abort();
    if (signal?.aborted) controller.abort(); else signal?.addEventListener('abort', cancel, { once: true });
    const timeout = {
      '/api/describe': 100000,
      '/api/chat': 90000,
      '/api/recording/stop': 40000,
      '/api/transcribe': 40000,
      '/api/playback/stop': 5000
    }[path] || 25000;
    const timer = setTimeout(() => controller.abort(), timeout);
    try {
      const headers = options.body instanceof FormData ? {} : options.body ? { 'Content-Type': 'application/json' } : {};
      const response = await fetch(endpoint(path, base), { ...options, headers: { ...headers, ...options.headers }, signal: controller.signal, cache: 'no-store' });
      if (!response.ok) { let detail = ''; try { const data = await response.json(); detail = typeof data.error === 'string' ? data.error : ''; } catch {} throw new Error(detail || `服务返回错误（${response.status}），请检查电脑端服务。`); }
      let data; try { data = await response.json(); } catch { throw new Error('服务没有返回有效的 JSON 数据。'); }
      if (!data || typeof data !== 'object') throw new Error('服务返回的数据格式不正确。');
      return data;
    } catch (error) {
      if (error.name === 'AbortError') throw new Error(signal?.aborted ? '操作已取消。' : '服务响应超时，请检查服务是否运行。');
      if (error instanceof TypeError) throw new Error('无法连接服务，请确认地址、服务运行状态和跨域访问设置。');
      throw error;
    } finally { clearTimeout(timer); signal?.removeEventListener('abort', cancel); }
  }
  function validStatus(data) {
    if (typeof data.device?.connected !== 'boolean' || typeof data.model?.connected !== 'boolean') throw new Error('服务状态格式不正确：需要 device.connected 和 model.connected。');
    return data;
  }
  function refreshConnection() {
    const connected = serviceStatus?.device?.connected === true;
    const model = serviceStatus?.model?.connected === true;
    const deviceText = connected ? '音箱已连接' : serviceStatus?.device?.checked === false ? '等待检测音箱' : '音箱未连接';
    const modelText = model ? '模型已连接' : serviceStatus?.model?.configured ? '模型已配置' : '模型未连接';
    $('connection-label').classList.toggle('connected', connected);
    $('connection-label').replaceChildren(); const dot = document.createElement('span'); dot.className = 'status-dot';
    dot.setAttribute('aria-hidden', 'true');
    $('connection-label').append(dot, document.createTextNode(deviceText));
    $('sidebar-connection').classList.toggle('connected', connected);
    const sidebarStatus = $('sidebar-device');
    if (sidebarStatus.textContent !== deviceText) sidebarStatus.textContent = deviceText;
    $('footer-status').textContent = `${deviceText} · ${modelText}`;
  }
  function applyMode() {
    const [audioSource, audioNote] = {
      demo: ['演示输入', '演示模式使用示例文本，不会开启麦克风。'],
      browser: ['电脑麦克风', '可以说“描述一下你看到的画面”。浏览器识别可能使用在线服务。'],
      service: ['音箱采集服务', '开始录音后，说“描述一下你看到的画面”，然后结束录音。']
    }[config.mode];
    $('audio-source-label').textContent = audioSource;
    $('audio-note').textContent = audioNote;
    $('describe-help').textContent = '发送当前预览的一帧给 DeepSeek。未开启预览时，使用音箱摄像头拍照。';
    $('audio-status').textContent = '准备好，开始说话'; refreshConnection();
  }
  function stopSpeech() {
    ++speechVersion;
    replyAudio.onended = null;
    replyAudio.onerror = null;
    replyAudio.pause();
    replyAudio.currentTime = 0;
    window.speechSynthesis?.cancel();
    const stopServer = serverPlayback || descriptionInProgress;
    descriptionInProgress = false;
    finishSpeech();
    if (stopServer) {
      const stop = api('/api/playback/stop', {
        method: 'POST',
        body: '{}',
        keepalive: true
      }, localService).catch(() => {});
      pendingPlaybackStop = stop;
      stop.then(() => {
        if (pendingPlaybackStop === stop) pendingPlaybackStop = null;
      });
    }
    return pendingPlaybackStop;
  }
  function finishSpeech() {
    clearTimeout(serverPlaybackTimer);
    serverPlayback = false;
    speechActive = false;
    $('speak-answer').innerHTML = `${icon('volume')}<span>朗读回答</span>`;
    setFlow('');
  }
  function markPlaying() {
    speechActive = true;
    setFlow('speak');
    $('speak-answer').innerHTML = `${icon('stop')}<span>停止播报</span>`;
  }
  function discardServiceRecording() {
    const capture = serviceCapture;
    serviceCapture = null;
    if (!capture) return;
    api('/api/recording/stop', {
      method: 'POST',
      body: JSON.stringify({ session_id: capture.sessionId, discard: true }),
      keepalive: true
    }, capture.base).catch(() => {});
  }
  function speakNotice(message) {
    if (!window.speechSynthesis) return;
    const utterance = new SpeechSynthesisUtterance(message);
    utterance.lang = 'zh-CN';
    window.speechSynthesis.speak(utterance);
  }
  async function cancelWork() {
    ++generation; requestController?.abort(); requestController = null;
    if (recognition) { recognition.onend = null; recognition.onresult = null; recognition.onerror = null; try { recognition.abort(); } catch {} recognition = null; }
    discardServiceRecording();
    announceDescription('');
    setRecording(false); setBusy(false); stopSpeech();
  }
  function remember(question, response, kind) {
    const item = { id: currentId, text: question, answer: response, mode: kind, time: Date.now() };
    history = [item, ...history.filter((old) => old.id !== currentId)].slice(0, 30);
    if (!storage.set('connect-studio-history', history)) toast('浏览器未允许保存记录，本次对话仍可导出。');
    renderHistory();
  }
  function renderHistory() {
    $('history-count').textContent = String(history.length); $('recent-list').replaceChildren(); $('history-list').replaceChildren();
    if (!history.length) {
      const hint = document.createElement('p'); hint.className = 'history-empty'; hint.innerHTML = '从一句话开始，<br>让想法被听见。'; $('recent-list').append(hint);
      const empty = document.createElement('p'); empty.className = 'field-help'; empty.textContent = '还没有对话记录。完成一轮对话后，会自动保存在这里。'; $('history-list').append(empty); return;
    }
    history.forEach((item, index) => {
      if (index < 7) { const button = document.createElement('button'); button.className = 'recent-item'; button.innerHTML = icon('chat'); const label = document.createElement('span'); label.textContent = item.text; button.append(label); button.title = item.text; button.addEventListener('click', () => loadHistory(item)); $('recent-list').append(button); }
      const row = document.createElement('div'); row.className = 'history-entry';
      const button = document.createElement('button'); button.className = 'history-open'; const title = document.createElement('strong'); title.textContent = item.text;
      const meta = document.createElement('span'); meta.textContent = `${new Date(item.time).toLocaleString('zh-CN', { month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit' })} · ${item.mode === 'demo' ? '演示对话' : '模型对话'}`;
      button.append(title, meta); button.addEventListener('click', () => loadHistory(item));
      const remove = document.createElement('button'); remove.className = 'icon-button'; remove.innerHTML = icon('close'); remove.setAttribute('aria-label', `删除对话：${item.text.slice(0, 25)}`);
      remove.addEventListener('click', () => { history = history.filter((old) => old.id !== item.id); storage.set('connect-studio-history', history); renderHistory(); });
      row.append(button, remove); $('history-list').append(row);
    });
  }
  async function loadHistory(item) {
    await cancelWork(); currentId = item.id; answerQuestion = item.text;
    setTranscript(item.text, '历史记录'); showAnswer(item.answer, item.mode);
    $('audio-status').textContent = '已载入历史对话'; $('history-dialog').close();
    selectMobilePanel('answer', true);
  }
  function demoResponse(question) {
    if (/介绍|做什么|你好|能力/.test(question)) return initialAnswer;
    if (/画面|看到|摄像|桌面|拍照/.test(question)) return '当前这段是演示回答。\n\n选择“描述当前画面”，可以拍一张照片，发送给 DeepSeek 生成语音描述。';
    if (/计划|安排|工作|学习/.test(question)) return '可以，我们把事情拆成更容易开始的几步。\n\n1. 先确定今天最重要的一件事。\n2. 为它安排一段不被打扰的时间。\n3. 把剩余任务分成“今天做”和“以后做”。\n4. 结束前花几分钟回顾进展。\n\n这是预设演示内容。连接模型服务后，我会根据你的具体问题生成回答。';
    return `我收到了你的问题：\n“${question}”\n\n这里演示的是“输入 → 识别文字 → 回答”的完整交互。你可以修改上方文字、查看历史记录，或点击朗读听听回答。\n\n当前没有调用大模型。这段内容是页面示例，连接模型服务后将显示真实回答。`;
  }
  function isDescribeCommand(text) {
    const command = text.replace(/[\s，。！？、,.!?]/g, '')
      .replace(/^(?:请问|请你|请您|请|帮我|麻烦你|麻烦|能不能|可以){1,3}/, '')
      .replace(/[吧吗]$/, '');
    return /^(?:描述|介绍|说说|讲讲)(?:一下)?(?:[你您](?:现在)?(?:所)?看到的|当前的?|眼前的?|前方的?|周围的?)?(?:画面|场景|景象|环境)$/.test(command)
      || /^(?:看看|看一下|告诉我)?(?:前面|前方|眼前|周围|桌上|桌面上|镜头里|画面里)(?:有|都有)什么$/.test(command)
      || /^(?:告诉我)?你(?:现在)?看到(?:了)?什么$/.test(command);
  }
  function announceDescription(message, error = false) {
    const status = $('describe-status');
    status.textContent = message;
    status.dataset.state = error ? 'error' : 'status';
  }
  function capturePreviewPhoto() {
    if (cameraPending) throw new Error('摄像头画面尚未准备好，请稍后重试。');
    if (!cameraKind) return '';
    const image = cameraKind === 'service-image';
    const source = $(image ? 'camera-image' : 'camera-video');
    const width = image ? source.naturalWidth : source.videoWidth;
    const height = image ? source.naturalHeight : source.videoHeight;
    const trackEnded = cameraKind === 'browser' && cameraStream?.getVideoTracks()[0]?.readyState !== 'live';
    if (trackEnded || !width || !height || (!image && source.readyState < 2)) {
      throw new Error('摄像头画面尚未准备好，请稍后重试。');
    }

    const scale = Math.min(1, 1600 / Math.max(width, height));
    const canvas = document.createElement('canvas');
    canvas.width = Math.round(width * scale);
    canvas.height = Math.round(height * scale);
    try {
      canvas.getContext('2d').drawImage(source, 0, 0, canvas.width, canvas.height);
      const sample = document.createElement('canvas');
      sample.width = 48;
      sample.height = 27;
      const context = sample.getContext('2d');
      context.drawImage(canvas, 0, 0, sample.width, sample.height);
      const pixels = context.getImageData(0, 0, sample.width, sample.height).data;
      let visiblePixels = 0;
      for (let i = 0; i < pixels.length; i += 4) {
        if (Math.max(pixels[i], pixels[i + 1], pixels[i + 2]) >= 36) visiblePixels++;
      }
      if (visiblePixels <= sample.width * sample.height / 100) {
        throw new Error('预览画面太暗，未发送给 DeepSeek。请检查镜头盖和光线后再试。');
      }
      return canvas.toDataURL('image/jpeg', 0.88);
    } catch (error) {
      if (error.name === 'SecurityError') {
        throw new Error('当前视频地址限制跨域取图。请使用同源视频地址，或在画面来源中选择电脑 / USB 摄像头。');
      }
      throw error;
    }
  }
  function localMediaUrl(value) {
    if (typeof value !== 'string' || !value) return '';
    try {
      const media = new URL(value, localService);
      if (media.origin === localService && media.pathname.startsWith('/api/media/')) return media.href;
    } catch {}
    return '';
  }
  function showDescriptionPhoto(source) {
    if (!source) return;
    const image = $('description-image');
    const photo = $('description-photo');
    image.onload = () => { photo.hidden = false; };
    image.onerror = () => { photo.hidden = true; };
    image.src = source;
  }
  async function describeScene(question = describeQuestion, fromVoice = false) {
    if (busy) return;
    if (!localService) {
      const message = '请从本地启动器打开页面，再使用画面描述。';
      announceDescription(message, true);
      if (fromVoice) speakNotice(message);
      return;
    }
    const stopping = stopSpeech();
    clearDescriptionMedia();
    const ticket = ++generation;
    requestController = new AbortController();
    $('answer-content').setAttribute('aria-live', 'off');
    setBusy(true);
    setFlow('answer');
    announceDescription('正在拍照并生成语音描述。');
    $('answer-badge').textContent = '正在描述画面';
    $('answer-badge').className = 'badge amber';
    $('answer-content').className = 'answer-content plain-answer busy-dots';
    $('answer-content').textContent = '等待 DeepSeek 描述画面';
    $('copy-answer').disabled = true;

    try {
      if (stopping) await stopping;
      if (ticket !== generation) return;
      const image = capturePreviewPhoto();
      const body = { text: question };
      if (image) body.image = image;
      $('answer-scroll').scrollTop = 0;
      selectMobilePanel('answer', true);
      descriptionInProgress = true;
      const response = await api('/api/describe', {
        method: 'POST',
        signal: requestController.signal,
        body: JSON.stringify(body)
      }, localService);
      if (ticket !== generation) return;
      if (typeof response.answer !== 'string' || !response.answer.trim()) {
        throw new Error('DeepSeek 没有返回画面描述。');
      }

      descriptionInProgress = false;
      answerQuestion = question;
      showAnswer(response.answer, 'model', response.model || 'DeepSeek Flash');
      answerAudioUrl = localMediaUrl(response.audio_url);
      showDescriptionPhoto(localMediaUrl(response.image_url) || image);
      $('answer-badge').textContent = '画面描述';
      remember(question, response.answer, 'model');
      setBusy(false);
      if (response.audio_deferred === true) {
        $('description-audio-status').hidden = false;
        announceDescription('预警语音优先，画面描述可稍后手动朗读。');
        return;
      }
      announceDescription(answerAudioUrl ? '画面描述已就绪。' : '画面描述已就绪，使用浏览器朗读。');
      if (response.played === true) {
        serverPlayback = true;
        markPlaying();
        const duration = response.audio_duration_ms;
        const estimate = Math.max(3500, Array.from(answer).length * 350);
        serverPlaybackTimer = setTimeout(finishSpeech, Number.isFinite(duration) && duration > 0 ? duration : estimate);
      } else {
        await speakAnswer(true);
      }
    } catch (error) {
      if (ticket !== generation) return;
      if (descriptionInProgress) stopSpeech();
      emptyAnswer(error.message);
      $('answer-badge').textContent = '画面描述失败';
      $('answer-badge').className = 'badge amber';
      announceDescription(error.message, true);
      if (fromVoice) speakNotice(error.message);
    } finally {
      if (ticket === generation) {
        setBusy(false);
        if (!speechActive) setFlow('');
        requestController = null;
        checkSavedService();
      }
    }
  }
  async function sendQuestion() {
    if (busy) return;
    const question = $('transcript').value.trim(); if (!question) { toast('先说一句话，或在识别文字区域输入问题。'); $('transcript').focus(); return; }
    if (isDescribeCommand(question)) return describeScene(question);
    if (config.mode !== 'demo' && !config.serviceUrl && !localService) { toast('请从本地启动器打开页面，或设置电脑端服务地址。'); openSettings(); return; }
    stopSpeech(); const ticket = ++generation;
    $('answer-content').setAttribute('aria-live', 'polite');
    setBusy(true); setFlow('answer');
    $('answer-badge').textContent = config.mode === 'demo' ? '演示生成中' : '正在思考'; $('answer-badge').className = 'badge amber';
    $('answer-content').className = 'answer-content plain-answer busy-dots'; $('answer-content').textContent = config.mode === 'demo' ? '正在准备演示回答' : '正在等待模型回答';
    $('answer-scroll').scrollTop = 0;
    selectMobilePanel('answer', true);
    $('copy-answer').disabled = true; $('speak-answer').disabled = true;
    try {
      let result, model = '', kind = config.mode === 'demo' ? 'demo' : 'model';
      if (kind === 'demo') {
        await sleep(500); if (ticket !== generation) return;
        result = demoResponse(question); $('answer-content').classList.remove('busy-dots');
        for (let index = 0; index < result.length; index += 5) {
          if (ticket !== generation) return; $('answer-content').textContent = result.slice(0, index + 5); await sleep(16);
        }
      } else {
        requestController = new AbortController();
        const resultData = await api('/api/chat', { method: 'POST', signal: requestController.signal, body: JSON.stringify({ text: question, session_id: currentId }) });
        if (typeof resultData.answer !== 'string' || !resultData.answer.trim()) throw new Error('模型服务没有返回回答文字。');
        result = resultData.answer; model = typeof resultData.model === 'string' ? resultData.model : '';
      }
      if (ticket !== generation) return;
      answerQuestion = question; showAnswer(result, kind, model); remember(question, result, kind); $('audio-status').textContent = '回答已就绪，可以继续提问';
    } catch (error) {
      if (ticket !== generation) return; emptyAnswer(error.message); $('answer-badge').textContent = '未能获取回答'; $('answer-badge').className = 'badge amber'; toast(error.message);
    } finally { if (ticket === generation) { setBusy(false); setFlow(''); requestController = null; } }
  }
  async function demoRecording() {
    const ticket = ++generation; stopSpeech(); setRecording(true); setBusy(true);
    setTranscript('', '演示识别中'); $('audio-status').textContent = '正在展示示例识别过程';
    for (let index = 0; index < exampleQuestion.length; index++) { if (ticket !== generation || !recording) return; setTranscript(exampleQuestion.slice(0, index + 1), '演示识别中'); await sleep(85); }
    if (ticket !== generation) return; setRecording(false); setBusy(false); setFlow('read'); $('audio-status').textContent = '示例文字已就绪'; $('transcript-badge').textContent = '示例文字';
  }
  async function browserRecording() {
    const SpeechRecognition = window.SpeechRecognition || window.webkitSpeechRecognition;
    if (!SpeechRecognition) { toast('这个浏览器不支持语音识别。可直接输入文字，或通过服务导入音频。'); return; }
    const stopping = stopSpeech();
    const ticket = ++generation;
    setBusy(true);
    if (stopping) await stopping;
    if (ticket !== generation) return;
    const recognizer = new SpeechRecognition();
    recognition = recognizer;
    recognizer.lang = 'zh-CN'; recognizer.continuous = true; recognizer.interimResults = true;
    let finalText = '', failed = false, commandDetected = false;
    recognizer.onstart = () => { if (ticket !== generation) { recognizer.abort(); return; } setRecording(true); setBusy(true); setTranscript('', '正在聆听'); $('audio-status').textContent = '正在聆听，请自然说话'; };
    recognizer.onresult = (event) => {
      if (ticket !== generation) return; let partial = '';
      for (let index = event.resultIndex; index < event.results.length; index++) { if (event.results[index].isFinal) finalText += event.results[index][0].transcript; else partial += event.results[index][0].transcript; }
      setTranscript(finalText + partial, partial ? '识别中' : '已识别');
      if (!partial && !commandDetected && isDescribeCommand(finalText)) {
        commandDetected = true;
        try { recognizer.stop(); } catch {}
      }
    };
    recognizer.onerror = (event) => {
      if (ticket !== generation) return; failed = true;
      const messages = { 'not-allowed': '麦克风权限未开启，请在浏览器中允许使用麦克风。', 'audio-capture': '找不到可用的麦克风，请检查电脑输入设备。', 'network': '浏览器识别服务不可用。可改用音箱服务或直接输入文字。', 'no-speech': '还没有听清，请靠近麦克风后重试。', 'service-not-allowed': '浏览器语音识别服务未获许可，请改用文字或音箱服务。' };
      $('audio-status').textContent = '语音识别未完成'; toast(messages[event.error] || '语音识别已中断，请重试。');
    };
    recognizer.onend = () => {
      if (ticket !== generation) return;
      recognition = null;
      setRecording(false);
      setBusy(false);
      setFlow('read');
      const text = $('transcript').value.trim();
      $('transcript-badge').textContent = text ? '识别完成' : '等待输入';
      if (failed) return;
      if (isDescribeCommand(finalText)) {
        setTranscript(finalText.trim(), '识别完成');
        $('audio-status').textContent = '已收到画面描述指令';
        describeScene(finalText.trim(), true);
      } else {
        $('audio-status').textContent = text ? '识别完成，可修改后发送' : '还没有识别到文字';
      }
    };
    try { setBusy(true); $('audio-status').textContent = '等待麦克风权限'; recognizer.start(); } catch { recognition = null; setBusy(false); toast('无法开启语音识别，请检查浏览器和麦克风权限。'); }
  }
  async function serviceRecordingStart() {
    const base = config.serviceUrl || localService;
    if (!base) { toast('请从本地启动器打开页面，或设置电脑端服务地址。'); openSettings(); return; }
    const stopping = stopSpeech();
    const ticket = ++generation;
    setBusy(true);
    $('audio-status').textContent = '正在启动音箱录音';
    try {
      if (stopping) await stopping;
      if (ticket !== generation) return;
      serviceCapture = { base, sessionId: currentId, local: base === localService };
      requestController = new AbortController();
      const data = await api('/api/recording/start', {
        method: 'POST',
        body: JSON.stringify({ session_id: serviceCapture.sessionId }),
        signal: requestController.signal
      }, base);
      if (ticket !== generation) return; if (data.recording !== true) throw new Error('设备服务未确认录音开始。');
      setTranscript('', '正在聆听');
      setRecording(true);
      setBusy(true);
      $('audio-status').textContent = '音箱录音已开始，可以说话。';
    } catch (error) {
      if (ticket !== generation) return;
      discardServiceRecording();
      setBusy(false);
      $('audio-status').textContent = '录音未能启动';
      toast(error.message);
      speakNotice(error.message);
    }
  }
  async function endRecording() {
    if (config.mode === 'demo') { ++generation; setRecording(false); setBusy(false); setFlow('read'); $('transcript-badge').textContent = '示例文字'; $('audio-status').textContent = '演示已停止'; return; }
    if (recognition) { try { recognition.stop(); } catch {} $('audio-status').textContent = '正在完成识别'; return; }
    const capture = serviceCapture;
    if (config.mode !== 'service' || !capture) return;
    const ticket = generation;
    setRecording(false);
    setBusy(true);
    $('audio-status').textContent = '正在识别录音';
    requestController = new AbortController();
    let descriptionCommand = '';
    try {
      const data = await api('/api/recording/stop', {
        method: 'POST',
        body: JSON.stringify({ session_id: capture.sessionId }),
        signal: requestController.signal
      }, capture.base);
      if (ticket !== generation) return;
      if (typeof data.transcript === 'string') setTranscript(data.transcript, '识别完成');
      const transcript = $('transcript').value.trim();
      if (!transcript) throw new Error('录音已结束，但服务没有返回识别文字。');
      if (isDescribeCommand(transcript)) descriptionCommand = transcript;
      $('audio-status').textContent = descriptionCommand ? '已收到画面描述指令' : '识别完成，可修改后发送';
      setFlow('read');
    } catch (error) {
      if (ticket !== generation) return;
      toast(error.message);
      speakNotice(error.message);
      $('audio-status').textContent = '识别未完成';
    } finally {
      if (ticket === generation) {
        serviceCapture = null;
        requestController = null;
        setBusy(false);
      }
    }
    if (descriptionCommand && ticket === generation) describeScene(descriptionCommand, true);
  }
  async function importAudio(file) {
    if (!file) return;
    if (file.size > 30 * 1024 * 1024) { toast('请选择小于 30 MB 的音频文件。'); return; }
    if (audioObjectUrl) URL.revokeObjectURL(audioObjectUrl); audioObjectUrl = URL.createObjectURL(file);
    $('audio-preview').src = audioObjectUrl; $('audio-preview').hidden = false;
    $('audio-status').textContent = `已导入：${file.name}`; $('audio-source-label').textContent = '本地音频';
    if (config.mode === 'demo' || (!config.serviceUrl && !localService)) { toast('音频已导入，可在下方回听。真实转写需在连接设置中切换模式并接入服务。'); return; }
    const ticket = ++generation; setBusy(true); setFlow('read'); $('transcript-badge').textContent = '识别中'; requestController = new AbortController();
    try {
      const body = new FormData(); body.append('audio', file); body.append('session_id', currentId);
      const data = await api('/api/transcribe', { method: 'POST', body, signal: requestController.signal });
      if (ticket !== generation) return;
      if (typeof data.text !== 'string' || !data.text.trim()) throw new Error('服务没有返回识别文字。');
      setTranscript(data.text, '识别完成'); $('audio-status').textContent = '音频识别完成，可修改后发送';
    } catch (error) { if (ticket === generation) { $('transcript-badge').textContent = '识别失败'; toast(error.message); } }
    finally { if (ticket === generation) setBusy(false); }
  }
  function openSettings() { $('mode-select').value = config.mode; $('service-url').value = config.serviceUrl; $('video-url').value = config.videoUrl; modeHelp(); if (!$('settings-dialog').open) $('settings-dialog').showModal(); }
  function modeHelp() {
    $('mode-help').textContent = { demo: '普通对话使用示例。描述画面会调用 DeepSeek。', browser: '使用浏览器默认麦克风，识别可能将音频发送到浏览器提供商。', service: '使用本地服务从音箱录音并转写。服务地址可留空。' }[$('mode-select').value];
  }
  function connectEvents() {
    eventSource?.close(); eventSource = null;
    if (!config.serviceUrl || config.serviceUrl === localService || config.mode === 'demo') return;
    try {
      const events = new EventSource(endpoint('/api/events')); eventSource = events;
      events.onmessage = (event) => {
        if (events !== eventSource) return;
        let data; try { data = JSON.parse(event.data); } catch { return; }
        if (data.session_id && data.session_id !== currentId) return;
        if (data.type === 'transcript' && typeof data.text === 'string' && recording) setTranscript(data.text, data.final ? '已识别' : '识别中');
        if (data.type === 'status') { try { serviceStatus = validStatus(data); refreshConnection(); } catch {} }
        if (data.type === 'error' && typeof data.message === 'string') toast(data.message);
      };
      events.onerror = () => { if (events !== eventSource) return; serviceStatus = null; refreshConnection(); $('footer-status').textContent = '实时状态连接中断 · 等待重连'; };
    } catch { /* Status checks and recording responses still work without optional SSE. */ }
  }
  async function checkSavedService() {
    const mode = config.mode;
    const base = mode === 'demo' ? localService : config.serviceUrl || localService;
    if (!base) return;
    const isCurrent = () => mode === config.mode && (mode === 'demo' || base === (config.serviceUrl || localService));
    try {
      const data = validStatus(await api('/api/status', {}, base));
      if (isCurrent()) { serviceStatus = data; refreshConnection(); connectEvents(); }
    } catch {
      if (isCurrent()) { serviceStatus = null; refreshConnection(); }
    }
  }
  function setCameraLoading(loading) { cameraPending = loading; $('start-camera').disabled = loading; $('camera-toggle').textContent = loading ? '取消连接' : cameraKind ? '关闭预览' : '开启预览'; }
  function stopCamera() {
    ++cameraVersion; clearTimeout(cameraTimeout); cameraStream?.getTracks().forEach((track) => track.stop()); cameraStream = null;
    const video = $('camera-video'), image = $('camera-image'); video.onloadedmetadata = null; video.onerror = null; image.onload = null; image.onerror = null;
    video.pause(); video.srcObject = null; video.removeAttribute('src'); video.load(); video.hidden = true; video.controls = false;
    video.defaultMuted = true; video.muted = true;
    image.removeAttribute('src'); image.hidden = true;
    if (videoObjectUrl) URL.revokeObjectURL(videoObjectUrl); videoObjectUrl = ''; cameraKind = '';
    $('camera-placeholder').hidden = false; $('video-stage').classList.remove('has-feed', 'has-playback');
    $('camera-state').textContent = '未开启'; $('camera-state').className = 'badge neutral'; $('camera-source-label').textContent = '尚未接入画面';
    $('video-dimensions').textContent = '等待视频源'; $('snapshot-button').disabled = true; $('feed-label').textContent = 'CAMERA PREVIEW'; setCameraLoading(false);
  }
  function cameraReady(kind, label, width, height) {
    clearTimeout(cameraTimeout); cameraKind = kind; setCameraLoading(false); $('camera-placeholder').hidden = true; $('video-stage').classList.add('has-feed');
    $('camera-state').textContent = kind === 'file' ? '本地回放' : '预览中'; $('camera-state').className = 'badge green';
    $('camera-source-label').textContent = label; $('video-dimensions').textContent = width && height ? `${width} × ${height}` : '实时画面';
    $('snapshot-button').disabled = false; $('feed-label').textContent = kind === 'file' ? 'LOCAL PLAYBACK' : 'LIVE PREVIEW';
  }
  function cameraFailure(message, ticket) { if (ticket !== cameraVersion) return; stopCamera(); $('camera-state').textContent = '连接失败'; $('camera-state').className = 'badge amber'; toast(message); }
  async function listCameras(selectedId) {
    if (!navigator.mediaDevices?.enumerateDevices) return;
    try {
      const devices = await navigator.mediaDevices.enumerateDevices(); const select = $('camera-source');
      select.querySelectorAll('[data-device]').forEach((option) => option.remove());
      devices.filter((device) => device.kind === 'videoinput').forEach((device, index) => { const option = document.createElement('option'); option.value = `device:${device.deviceId}`; option.textContent = device.label || `摄像头 ${index + 1}`; option.dataset.device = 'true'; select.append(option); });
      if (selectedId && Array.from(select.options).some((option) => option.value === `device:${selectedId}`)) select.value = `device:${selectedId}`;
    } catch {}
  }
  async function startCamera(event) {
    if (ringGestureArmed && event.detail > 0) $('workspace-main').focus({ preventScroll: true });
    if (cameraKind || cameraPending) { stopCamera(); return; }
    const selection = $('camera-source').value;
    if (selection === 'file') { $('video-file').click(); return; }
    if (selection === 'service') {
      if (!config.videoUrl) { toast('先在连接设置中填写音箱画面地址。'); openSettings(); return; }
      startUrlCamera(config.videoUrl); return;
    }
    if (!navigator.mediaDevices?.getUserMedia) { toast('当前浏览器不能访问摄像头，请通过 localhost 地址在 Edge 或 Chrome 中打开。'); return; }
    const ticket = ++cameraVersion; setCameraLoading(true); $('camera-state').textContent = '等待授权';
    try {
      const stream = await navigator.mediaDevices.getUserMedia({ video: selection.startsWith('device:') ? { deviceId: { exact: selection.slice(7) } } : { width: { ideal: 1280 }, height: { ideal: 720 } }, audio: false });
      if (ticket !== cameraVersion) { stream.getTracks().forEach((track) => track.stop()); return; }
      cameraStream = stream; const video = $('camera-video'); video.srcObject = stream; video.hidden = false;
      await video.play(); if (ticket !== cameraVersion) return;
      const track = stream.getVideoTracks()[0]; const settings = track.getSettings();
      cameraReady('browser', track.label || '电脑摄像头', video.videoWidth, video.videoHeight);
      track.onended = () => cameraFailure('摄像头已断开或权限被关闭。', ticket); await listCameras(settings.deviceId);
    } catch (error) {
      const messages = { NotAllowedError: '摄像头权限未开启，请在浏览器中允许使用摄像头。', NotFoundError: '没有找到摄像头，请接入 USB 摄像头或选择其他画面来源。', NotReadableError: '摄像头可能正被其他程序占用，请关闭占用程序后重试。', OverconstrainedError: '选择的摄像头不可用，请重新选择。' };
      cameraFailure(messages[error.name] || '无法打开摄像头，请检查设备和浏览器权限。', ticket);
    }
  }
  function startUrlCamera(value, localFile = null) {
    stopCamera(); const ticket = cameraVersion; let url;
    try { url = localFile ? URL.createObjectURL(localFile) : normalizeUrl(value, true); } catch (error) { toast(error.message); return; }
    if (localFile) videoObjectUrl = url;
    setCameraLoading(true); $('camera-state').textContent = '正在连接';
    const isVideo = localFile || /\.(mp4|webm|ogg|mov)(\?|$)/i.test(url);
    if (isVideo) {
      const video = $('camera-video');
      video.hidden = false; video.controls = true; video.loop = Boolean(localFile);
      video.defaultMuted = !localFile; video.muted = !localFile;
      $('video-stage').classList.add('has-playback');
      video.onloadedmetadata = () => { if (ticket !== cameraVersion) return; cameraReady(localFile ? 'file' : 'service-video', localFile ? localFile.name : '音箱视频服务', video.videoWidth, video.videoHeight); video.play().catch(() => { video.controls = true; toast('点击画面中的播放按钮开始预览。'); }); };
      video.onerror = () => cameraFailure('视频无法播放，请确认地址可用且编码受浏览器支持。', ticket); video.src = url;
    } else {
      const image = $('camera-image'); image.hidden = false;
      image.onload = () => { if (ticket === cameraVersion) cameraReady('service-image', '音箱视频服务', image.naturalWidth, image.naturalHeight); };
      image.onerror = () => cameraFailure('无法读取音箱画面，请确认视频服务地址可用。', ticket); image.src = url;
    }
    if (cameraPending) cameraTimeout = setTimeout(() => cameraFailure('画面连接超时，请确认视频服务已启动。', ticket), 15000);
  }
  function download(blob, filename) {
    const url = URL.createObjectURL(blob), link = document.createElement('a'); link.href = url; link.download = filename; link.click(); setTimeout(() => URL.revokeObjectURL(url), 2000);
  }
  function takeSnapshot() {
    if (!cameraKind) return;
    const source = cameraKind === 'service-image' ? $('camera-image') : $('camera-video'); const width = source.videoWidth || source.naturalWidth, height = source.videoHeight || source.naturalHeight;
    if (!width || !height) { toast('还没有可保存的画面，请稍后重试。'); return; }
    try {
      const canvas = document.createElement('canvas'); canvas.width = width; canvas.height = height; canvas.getContext('2d').drawImage(source, 0, 0);
      canvas.toBlob((blob) => { if (blob) { download(blob, `connect-frame-${Date.now()}.png`); toast('当前画面已保存。'); } else toast('无法保存画面，请重试。'); }, 'image/png');
    } catch { toast('该视频源限制了跨域截图。可在电脑端视频服务保存画面，或改用 USB 摄像头。'); }
  }
  function speakWithBrowser(ticket) {
    const synthesizer = window.speechSynthesis;
    if (!synthesizer) return false;
    synthesizer.cancel();
    const utterance = new SpeechSynthesisUtterance(answer);
    utterance.lang = 'zh-CN';
    const chineseVoice = synthesizer.getVoices().find((voice) => /^zh(?:[-_]|$)/i.test(voice.lang));
    if (chineseVoice) utterance.voice = chineseVoice;
    utterance.onend = () => {
      if (ticket === speechVersion) finishSpeech();
    };
    utterance.onerror = () => {
      if (ticket !== speechVersion) return;
      finishSpeech();
      const message = '浏览器未能播放语音。请检查 Windows 的声音输出，或点“朗读回答”重试。';
      if ($('answer-badge').textContent === '画面描述') announceDescription(message, true);
      else toast(message);
    };
    try {
      markPlaying();
      synthesizer.speak(utterance);
      return true;
    } catch {
      finishSpeech();
      return false;
    }
  }
  async function speakAnswer(automatic = false) {
    if (speechActive) { stopSpeech(); return; }
    if (!answer) return;
    const ticket = ++speechVersion;
    if (answerAudioUrl) {
      replyAudio.src = answerAudioUrl;
      replyAudio.onended = () => {
        if (ticket === speechVersion) finishSpeech();
      };
      try {
        markPlaying();
        await replyAudio.play();
        if (ticket !== speechVersion) return;
        replyAudio.onerror = () => {
          if (ticket !== speechVersion) return;
          finishSpeech();
          if (speakWithBrowser(ticket)) {
            announceDescription('音频暂时无法播放，正在使用浏览器朗读。');
          } else {
            announceDescription('音频暂时无法播放。请检查 Windows 的声音输出。', true);
          }
        };
        return;
      } catch (error) {
        if (ticket !== speechVersion) return;
        finishSpeech();
        if (automatic && error.name === 'NotAllowedError') {
          announceDescription('浏览器需要手动开始播放。点“朗读回答”即可听取描述。');
          return;
        }
      }
    }
    if (speakWithBrowser(ticket)) return;
    const message = '当前浏览器无法朗读，请检查系统语音和播放设备。';
    if (automatic) announceDescription(message, true);
    else toast(message);
  }
  async function newSession() {
    await cancelWork(); currentId = uid(); setTranscript('', '等待输入'); emptyAnswer(); $('recording-time').textContent = '00:00'; $('audio-status').textContent = '新对话已准备好';
    $('audio-preview').pause(); $('audio-preview').removeAttribute('src'); $('audio-preview').hidden = true;
    if (audioObjectUrl) URL.revokeObjectURL(audioObjectUrl); audioObjectUrl = '';
    selectMobilePanel('input'); $('transcript').focus();
  }

  // UI binding. All model and recognition text is inserted as text, never as HTML.
  $('transcript').addEventListener('input', () => { updateCount(); $('transcript-badge').textContent = '可发送'; });
  $('send-button').addEventListener('click', sendQuestion);
  $('mobile-conversation-input').addEventListener('click', () => selectMobilePanel('input'));
  $('mobile-conversation-answer').addEventListener('click', () => selectMobilePanel('answer'));
  $('describe-button').addEventListener('click', () => {
    setTranscript(describeQuestion, '画面描述指令');
    describeScene();
  });
  ringGestureToggle.disabled = !localService;
  ringGestureToggle.setAttribute('aria-pressed', String(ringGestureArmed));
  ringGestureToggle.addEventListener('click', () => {
    ringGestureArmed = !ringGestureArmed;
    ringGestureToggle.setAttribute('aria-pressed', String(ringGestureArmed));
    if (ringGestureArmed) $('workspace-main').focus({ preventScroll: true });
    toast(ringGestureArmed ? '戒指双击已启用，请保持声境页面在前台。' : '戒指双击已停用。');
  });
  $('record-button').addEventListener('click', () => { if (recording) endRecording(); else if (!busy) { if (config.mode === 'demo') demoRecording(); else if (config.mode === 'browser') browserRecording(); else serviceRecordingStart(); } });
  $('upload-button').addEventListener('click', () => { $('audio-file').value = ''; $('audio-file').click(); });
  $('audio-file').addEventListener('change', () => importAudio($('audio-file').files[0]));
  $('video-file').addEventListener('change', () => { const file = $('video-file').files[0]; if (file) startUrlCamera('', file); $('video-file').value = ''; });
  ['settings-button', 'sidebar-settings'].forEach((id) => $(id).addEventListener('click', openSettings));
  document.querySelectorAll('[data-close]').forEach((button) => button.addEventListener('click', () => $(button.dataset.close).close()));
  $('mode-select').addEventListener('change', modeHelp);
  $('test-connection').addEventListener('click', async () => {
    $('test-connection').disabled = true; $('settings-result').className = 'settings-result'; $('settings-result').textContent = '正在检测服务…';
    try {
      const base = normalizeUrl($('service-url').value) || localService;
      const data = validStatus(await api('/api/status', {}, base));
      const device = data.device.connected ? '音箱已连接' : data.device.checked === false ? '等待检测音箱' : '设备未连接';
      const model = data.model.connected ? '模型已连接' : data.model.configured ? '模型已配置' : '模型未连接';
      $('settings-result').textContent = `服务可用 · ${device} · ${model}`;
    }
    catch (error) { $('settings-result').className = 'settings-result error'; $('settings-result').textContent = error.message; }
    finally { $('test-connection').disabled = false; }
  });
  $('settings-form').addEventListener('submit', async (event) => {
    event.preventDefault();
    try {
      const next = { mode: $('mode-select').value, serviceUrl: normalizeUrl($('service-url').value), videoUrl: normalizeUrl($('video-url').value) };
      await cancelWork(); eventSource?.close(); eventSource = null;
      if (config.videoUrl !== next.videoUrl && cameraKind.startsWith('service')) stopCamera();
      config = next; storage.set('connect-studio-config', config); serviceStatus = null; applyMode(); $('settings-dialog').close();
      if (config.mode !== 'demo' && answerKind === 'demo') { emptyAnswer('连接模型后，你的回答会显示在这里。'); setTranscript('', '等待输入'); }
      toast('设置已保存。'); checkSavedService();
    } catch (error) { $('settings-result').className = 'settings-result error'; $('settings-result').textContent = error.message; }
  });
  $('start-camera').addEventListener('click', startCamera); $('camera-toggle').addEventListener('click', startCamera);
  $('camera-source').addEventListener('change', stopCamera); $('snapshot-button').addEventListener('click', takeSnapshot);
  $('fullscreen-button').addEventListener('click', async () => { try { if (document.fullscreenElement) await document.exitFullscreen(); else await $('video-stage').requestFullscreen(); } catch { toast('当前浏览器不支持全屏预览。'); } });
  $('speak-answer').addEventListener('click', () => speakAnswer());
  $('copy-answer').addEventListener('click', async () => { if (!answer) return; try { await navigator.clipboard.writeText(answer); toast('回答已复制。'); } catch { toast('浏览器未允许自动复制，可以选中回答文字手动复制。'); } });
  $('export-button').addEventListener('click', () => {
    if (!$('transcript').value.trim() && !answer) { toast('当前对话还没有内容。'); return; }
    const draft = $('transcript').value;
    const text = `声境 Connect · 对话记录\n时间：${new Date().toLocaleString('zh-CN')}\n内容来源：${answerKind === 'demo' ? '演示示例，并非真实模型回答' : '模型服务'}\n\n你说的话\n${answer ? answerQuestion : draft}\n\nAI 的回答\n${answer || '尚无回答'}\n${answer && draft !== answerQuestion ? `\n尚未回答的新输入\n${draft}\n` : ''}`;
    download(new Blob(['\uFEFF', text], { type: 'text/plain;charset=utf-8' }), `connect-conversation-${Date.now()}.txt`); toast('对话已导出。');
  });
  $('new-session').addEventListener('click', newSession);
  $('mobile-new').addEventListener('click', newSession);
  $('mobile-history').addEventListener('click', () => { renderHistory(); $('history-dialog').showModal(); });
  $('history-nav').addEventListener('click', () => { renderHistory(); $('history-dialog').showModal(); });
  $('clear-history').addEventListener('click', () => { if (!history.length) return; history = []; storage.set('connect-studio-history', history); renderHistory(); toast('本机对话记录已清空。'); });
  document.addEventListener('connect:ring-opening', () => {
    if (cameraPending || (cameraKind && cameraKind !== 'file')) stopCamera();
  });
  document.addEventListener('keydown', (event) => {
    // The ring sends one Enter through the Windows keyboard connection.
    if (ringGestureArmed && event.key === 'Enter' && !event.ctrlKey && !event.metaKey && !event.altKey && !event.shiftKey) {
      if (busy || event.defaultPrevented || event.repeat || event.isComposing || event.keyCode === 229
        || document.querySelector('dialog[open]') || event.target?.closest?.(interactiveTarget)
        || document.activeElement?.closest(interactiveTarget)) return;
      event.preventDefault();
      if ($('conversation-view').hidden) {
        $('workspace-nav').click();
        $('workspace-main').focus({ preventScroll: true });
      }
      setTranscript(describeQuestion, '戒指双击指令');
      describeScene(describeQuestion);
      return;
    }
    if ($('conversation-view').hidden) return;
    const editing = /INPUT|TEXTAREA|SELECT/.test(document.activeElement?.tagName);
    if ((event.ctrlKey || event.metaKey) && event.key === 'Enter' && !$('settings-dialog').open && !$('history-dialog').open) { event.preventDefault(); sendQuestion(); }
    if (!editing && !event.ctrlKey && !event.metaKey && !event.altKey && event.key.toLowerCase() === 'n' && !document.querySelector('dialog[open]')) { event.preventDefault(); newSession(); }
  });
  window.addEventListener('beforeunload', () => {
    discardServiceRecording();
    stopSpeech();
    ++generation; requestController?.abort(); recognition?.abort(); eventSource?.close(); cameraStream?.getTracks().forEach((track) => track.stop());
  });

  for (let index = 0; index < 65; index++) { const bar = document.createElement('span'); const height = 5 + Math.abs(Math.sin(index * 1.72) * Math.sin(index * .31)) * 30; bar.style.setProperty('--bar-height', `${height}px`); bar.style.setProperty('--bar-delay', `${-(index % 9) * .09}s`); $('waveform').append(bar); }
  updateCount(); renderHistory(); applyMode();
  if (config.mode !== 'demo') { setTranscript('', '等待输入'); emptyAnswer(); }
  checkSavedService();
})();
