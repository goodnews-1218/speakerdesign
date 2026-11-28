(() => {
  'use strict';

  const $ = (id) => document.getElementById(id);
  const panel = $('ring-monitor');
  const form = $('ring-form');
  const demo = $('ring-demo-input');
  const analyzeDemo = $('ring-demo-analysis');
  const baselineMode = $('ring-baseline-mode');
  const baselineInput = $('ring-baseline-input');
  const preview = $('ring-preview');
  const previewPulse = $('ring-preview-pulse');
  const video = $('ring-video');
  const stateLabels = {
    idle: '未启动',
    starting: '正在启动',
    running: '监测中',
    stopping: '正在停止',
    stopped: '已停止',
    error: '运行异常',
  };
  const pulseLabels = {
    TRACKING: '正在接收戒指读数',
    RECOVERED: '信号较弱，显示估算读数',
    HOLDING: '读数暂缓更新',
    LAST_KNOWN: '最后一次有效读数',
    UNAVAILABLE: '暂无有效读数',
    WAITING_FOR_PULSE: '正在等待心率读数',
  };
  const verdicts = {
    suspected_fall: ['疑似跌倒或突然瘫软', 'danger'],
    suspected_immobility: ['预警后持续坐姿不动', 'danger'],
    acknowledged: ['已收到语音确认', 'clear'],
    no_visible_fall: ['未发现明显跌倒', 'clear'],
    uncertain: ['画面无法确认', 'pending'],
    demo: ['演示结果', 'pending'],
    already_alerted: ['本次预警已响铃', 'pending'],
    error: ['分析失败', 'danger'],
    cancelled: ['分析已停止', 'pending'],
    pending: ['等待分析', 'pending'],
  };
  const captureLabels = {
    ready: ['抽帧已保存', 'success'],
    insufficient: ['有效画面不足', 'warning'],
    failed: ['抽帧失败', 'error'],
    unknown: ['抽帧状态未记录', ''],
  };
  const analysisLabels = {
    queued: ['排队中，尚未提交', 'warning'],
    requesting: ['请求中，等待 DeepSeek 返回', 'warning'],
    received: ['分析已返回', 'success'],
    failed: ['分析请求失败', 'error'],
    skipped: ['未提交本批画面', ''],
    unknown: ['提交状态未记录', ''],
  };
  const alarmLabels = {
    pending: ['等待分析结果', 'warning'],
    confirming: ['等待语音确认', 'warning'],
    playing: ['正在通过 Windows 播放', 'warning'],
    played: ['已响铃', 'success'],
    failed: ['Windows 音频播放失败', 'error'],
    skipped: ['未触发警报声', ''],
    unknown: ['播放状态未记录', ''],
  };
  const alarmSkipLabels = {
    voice_acknowledged: '用户表示自己没事，本次未响铃',
    stopped: '监测已停止，未播放',
    no_fall: '分析结果未达到响铃条件',
    uncertain: '画面无法确认，未播放',
    demo: '模拟心率，未自动播放',
    already_alerted: '同一次预警已播放',
    insufficient_frames: '有效画面不足，未播放',
    analysis_failed: '分析失败，未播放',
  };
  const voiceLabels = {
    prompting: ['正在准备或播放询问', 'warning'],
    listening: ['正在听取回应（8 秒）', 'warning'],
    transcribing: ['正在识别回应', 'warning'],
    interpreting: ['DeepSeek 正在判断回应含义', 'warning'],
    acknowledged: ['用户表示自己没事', 'success'],
    needs_help: ['用户报告异常或求助', 'warning'],
    no_reply: ['窗口内未收到回应', 'warning'],
    unconfirmed: ['未听到明确的平安确认', 'warning'],
    unavailable: ['语音确认不可用，需人工查看', 'error'],
    stopped: ['语音确认已停止'],
  };

  let status = null;
  let actionPending = false;
  let testingAlarm = false;
  let connectionError = '';
  let actionError = '';
  let pollTimer;
  let pollController;
  let session = null;
  let previewSource = '';
  let previewAttempt = 0;
  let eventSignature = '';
  let eventAnnouncements = new Map();
  let historyItems = [];
  let historyCursor = null;
  let historyLoading = false;
  let historyNeeded = true;
  let videoSignature = '';
  let selectedVideo = '';

  function element(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text) node.textContent = text;
    return node;
  }

  function mediaUrl(value) {
    if (typeof value !== 'string' || !value) return '';
    try {
      const url = new URL(value, location.href);
      if (url.origin === location.origin && url.pathname.startsWith('/api/ring/')) {
        return url.pathname + url.search;
      }
    } catch {
      return '';
    }
    return '';
  }

  function isActive(state) {
    return ['starting', 'running', 'stopping'].includes(state);
  }

  function renderState(state, label = stateLabels[state]) {
    const badge = $('ring-state');
    if (badge.textContent !== label) badge.textContent = label;
    badge.dataset.state = state;
  }

  function updateControls() {
    const state = status?.state;
    $('ring-start').disabled = actionPending || testingAlarm || isActive(state) || !status || !!connectionError;
    $('ring-stop').disabled = actionPending || !['starting', 'running'].includes(state);
    $('ring-test-alarm').disabled = actionPending || testingAlarm || isActive(state) || !status || !!connectionError;
    const locked = actionPending || isActive(state);
    const manual = baselineMode.value === 'manual';
    baselineMode.disabled = locked;
    $('ring-manual-baseline').hidden = !manual;
    baselineInput.disabled = locked || !manual;
    baselineInput.required = manual;
    demo.disabled = locked;
    $('ring-demo-analysis-option').hidden = !demo.checked;
    analyzeDemo.disabled = locked || !demo.checked;
    const help = !demo.checked
      ? '连接 BioScope 戒指。预警抽帧与语音确认片段会提交给 DeepSeek。'
      : analyzeDemo.checked
        ? '使用真实画面及语音，提交给 DeepSeek。未确认平安时会响铃。'
        : '仅保存本地录像与抽帧。';
    const helpNode = $('ring-start-help');
    if (helpNode.textContent !== help) helpNode.textContent = help;
    const message = actionError || connectionError || status?.error || '';
    const messageNode = $('ring-status-message');
    if (messageNode.textContent !== message) messageNode.textContent = message;
    messageNode.dataset.level = message ? 'error' : '';
    messageNode.hidden = !message;
  }

  function setDiagnostic(id, [label, tone = '']) {
    const node = $(id);
    if (node.textContent !== label) node.textContent = label;
    node.dataset.tone = tone;
  }

  function episodeNumber(item) {
    return Number(/^(?:event-)?(\d+)(?:-\d+)?$/.exec(String(item.id))?.[1]) || 0;
  }

  function eventStages(item) {
    const capture = captureLabels[item.capture_state] || captureLabels.unknown;
    const analysis = analysisLabels[item.analysis_state] || analysisLabels.unknown;
    let alarm = alarmLabels[item.alarm_state] || alarmLabels.unknown;
    if (item.alarm_played) alarm = alarmLabels.played;
    if (alarm === alarmLabels.skipped && Object.hasOwn(alarmSkipLabels, item.alarm_reason)) {
      alarm = [alarmSkipLabels[item.alarm_reason]];
    }
    return { capture, analysis, alarm, voice: voiceLabels[item.voice_state] || ['未开始'] };
  }

  function renderDiagnostic(data) {
    const events = Array.isArray(data?.events) ? data.events : [];
    const latest = events[0];
    const triggerCount = data?.warning_count || 0;
    const episode = latest ? episodeNumber(latest) : 0;
    const waitingForFrames = triggerCount > 0 && (!latest || (episode > 0 && triggerCount > episode));
    const collecting = ['starting', 'running'].includes(data?.state);
    let trigger = ['尚未启动心率监测'];
    let capture = ['等待心率预警'];
    let analysis = ['未提交'];
    let alarm = ['未触发'];
    let voice = ['未开始'];
    let detail = '';
    let detailTone = '';
    if (!data) {
      trigger = ['无法读取本机监测状态', 'error'];
      capture = analysis = alarm = ['状态暂不可用'];
    } else if (waitingForFrames) {
      trigger = [data.demo ? '已确认模拟预警' : '已确认心率预警', 'warning'];
      capture = data.capture_error ? ['抽帧出错', 'error']
        : isActive(data.state) ? ['正在准备预警画面', 'warning'] : ['暂无已保存画面', 'warning'];
      if (collecting && !data.capture_error) alarm = ['等待抽帧和分析', 'warning'];
      detail = data.capture_error || '';
    } else if (latest) {
      const stages = eventStages(latest);
      const lastBatch = latest.batches?.at(-1);
      const warning = data.demo ? '模拟预警' : '心率预警';
      trigger = [episode ? `第 ${episode} 次${warning}` : `已确认${warning}`, 'warning'];
      capture = stages.capture;
      analysis = ['queued', 'requesting'].includes(lastBatch?.analysis_state)
        ? eventStages(lastBatch).analysis : stages.analysis;
      alarm = stages.alarm;
      voice = stages.voice;
      if (latest.verdict !== 'pending') {
        const [verdict, tone] = verdicts[latest.verdict] || [];
        detail = [latest.urgent ? '高优先级：未收到回应，请立即查看现场' : verdict,
          latest.summary].filter(Boolean).join(' · ');
        detailTone = tone === 'danger' ? 'error' : '';
      }
    } else if (data.state === 'starting') {
      trigger = ['监测正在启动'];
    } else if (data.state === 'running') {
      const live = data.pulse_state === 'TRACKING' || (!data.pulse_state && data.bpm > 0);
      trigger = !live ? ['等待稳定的戒指心率']
        : ['auto', 'CALIBRATING'].includes(data.baseline) ? ['正在校准基线，尚未确认预警']
          : ['监测中，尚未确认预警'];
    } else if (['stopping', 'stopped', 'error'].includes(data.state)) {
      trigger = ['本轮未确认心率预警'];
    }
    if (data?.demo) {
      if (!data.analyze_demo) analysis = ['模拟心率，画面保存在本机'];
      else if ((!latest || waitingForFrames) && isActive(data.state)) analysis = ['已开启，等待预警画面'];
      if (!data.analyze_demo) alarm = ['本地演示，不响铃'];
    }
    if (connectionError && data) trigger = ['连接中断，显示最后一次状态', 'error'];
    setDiagnostic('ring-trigger-state', trigger);
    setDiagnostic('ring-capture-state', capture);
    setDiagnostic('ring-analysis-state', analysis);
    setDiagnostic('ring-alarm-state', alarm);
    setDiagnostic('ring-voice-state', voice);
    setDiagnostic('ring-diagnostic-detail', [detail, detailTone]);
    $('ring-diagnostic-detail').hidden = !detail;
    $('ring-view-events').hidden = !events.length;
    $('ring-diagnostic-review').hidden = !detail && !events.length;
    setDiagnostic('ring-audio-output', [isActive(data?.state)
      ? '警报使用 Windows 默认声音输出。停止监测后可试听。'
      : '警报使用 Windows 默认声音输出。实际能否听到，可点「试听警报」确认。']);
  }

  function formatBaseline(value) {
    if (value == null || value === '') return '—';
    const text = String(value);
    if (text.toUpperCase() === 'CALIBRATING') return '标定中';
    if (text.toLowerCase() === 'auto') return '自动';
    const range = /^(\d+(?:\.\d+)?)(?:\.\.|:|–)(\d+(?:\.\d+)?)$/.exec(text);
    if (range) return range[1] === range[2] ? range[1] + ' BPM' : range[1] + '–' + range[2] + ' BPM';
    return /^\d+(?:\.\d+)?$/.test(text) ? text + ' BPM' : text;
  }

  function renderPreviewPulse(data) {
    const { bpm, pulse_state: pulseState, demo } = data;
    const hasReading = Number.isFinite(bpm) && bpm > 0;
    const live = hasReading && (pulseState === 'TRACKING' || pulseState === 'RECOVERED');
    const note = !hasReading ? '等待读数' : {
      RECOVERED: '估算读数',
      HOLDING: '暂缓更新',
      LAST_KNOWN: '历史读数',
    }[pulseState] || '';
    $('ring-preview-bpm').textContent = hasReading ? String(Math.round(bpm)) : '—';
    $('ring-preview-pulse-note').textContent = [note, demo ? '演示' : ''].filter(Boolean).join(' · ');
    previewPulse.dataset.live = String(live);
    if (live) previewPulse.style.setProperty('--beat-duration', `${60 / bpm}s`);
    previewPulse.hidden = false;
  }

  function renderPreview(data) {
    const source = panel.hidden || document.hidden ? '' : mediaUrl(data?.preview_url);
    const stateText = $('ring-preview-state');
    if (source) renderPreviewPulse(data);
    else previewPulse.hidden = true;
    if (source && source === previewSource) return;
    previewSource = source;
    if (source) {
      stateText.textContent = '实时预览';
      preview.hidden = false;
      $('ring-preview-empty').hidden = true;
      preview.src = source + (source.includes('?') ? '&' : '?') + 'attempt=' + ++previewAttempt;
      return;
    }
    preview.removeAttribute('src');
    preview.hidden = true;
    $('ring-preview-empty').hidden = false;
    if (data?.state === 'starting') stateText.textContent = '正在连接音箱';
    else if (data?.state === 'running') stateText.textContent = '等待摄像头画面';
    else stateText.textContent = '暂无画面';
  }

  preview.addEventListener('error', () => {
    if (!previewSource) return;
    previewSource = '';
    preview.hidden = true;
    previewPulse.hidden = true;
    $('ring-preview-empty').hidden = false;
    $('ring-preview-state').textContent = '正在重连画面';
  });

  function frameTime(value, pivot) {
    if (!Number.isFinite(value)) return '片段画面';
    const offset = value - (pivot ?? 0);
    const name = pivot == null ? '确认' : '异常';
    if (offset === 0) return name + '时刻';
    return (offset < 0 ? name + '前 ' : name + '后 ') + (Math.abs(offset) / 1000).toFixed(1) + ' 秒';
  }

  const eventKey = (item) => `${item.session_id || session}/${item.id}`;

  function renderEvent(item, expanded, defaultOpen) {
    const [verdict, tone] = verdicts[item.verdict] || ['等待确认', 'pending'];
    const event = element('details', 'ring-event');
    const key = eventKey(item);
    event.dataset.eventId = key;
    event.open = expanded.get(key) ?? defaultOpen;
    const header = element('summary', 'ring-event-heading');
    const timestamp = new Date(item.created_at);
    const title = Number.isNaN(timestamp.getTime()) ? '心率预警' : timestamp.toLocaleString();
    header.append(
      element('span', 'ring-event-name', title + (item.demo ? ' · 模拟' : '')),
      element('span', 'ring-badge ring-verdict ' + tone, item.urgent ? '高优先级 · 未收到回应' : verdict),
    );
    const body = element('div', 'ring-event-body');
    appendEventResult(body, item);
    const pivot = Number.isFinite(item.onset_ms) ? item.onset_ms : null;
    const phases = ['before', 'after'].map((phase, index) => {
      const block = element('section', 'ring-phase');
      const stage = pivot == null ? '预警确认' : '心率异常';
      block.append(element('h3', 'ring-phase-title', stage + (index === 0 ? '前' : '后')));
      const notes = item[phase] || [];
      block.append(element('p', 'ring-phase-observation', notes.length ? notes.join(' ') : '这段暂无可用的画面描述。'));
      const frames = element('div', 'ring-frames');
      block.append(frames);
      body.append(block);
      return frames;
    });
    for (const frame of item.frames || []) {
      const source = mediaUrl(frame.url);
      if (!source) continue;
      const time = frameTime(frame.time_ms, pivot);
      const before = frame.time_ms < (pivot ?? 0);
      const link = element('a', 'ring-frame');
      link.href = source;
      link.target = '_blank';
      link.rel = 'noopener';
      const image = element('img');
      image.src = source;
      image.alt = '预警抽帧，' + time;
      image.loading = 'lazy';
      const label = element('span', 'ring-frame-time', time);
      if (Number.isFinite(frame.time_ms)) {
        label.dataset.phase = before ? 'before' : frame.time_ms === (pivot ?? 0) ? 'at' : 'after';
      }
      link.append(image, label);
      phases[before ? 0 : 1].append(link);
    }
    event.append(header, body);
    return event;
  }

  function appendEventResult(body, item) {
    if (item.summary) body.append(element('p', 'ring-event-summary', item.summary));
    const { alarm, voice } = eventStages(item);
    const stages = element('dl', 'ring-event-stages');
    for (const [label, value] of [['语音确认', voice], ['警报声', alarm]]) {
      stages.append(element('dt', '', label), element('dd', '', value[0]));
    }
    body.append(stages);
  }

  function renderEvents(data) {
    const current = Array.isArray(data.events) ? data.events : [];
    const combined = new Map(historyItems.map((item) => [eventKey(item), item]));
    for (const item of current) combined.set(eventKey(item), item);
    const items = [...combined.values()].sort((a, b) => String(b.created_at).localeCompare(String(a.created_at)));
    const signature = JSON.stringify(items);
    const empty = $('ring-events-empty');
    $('ring-event-count').textContent = items.length + (historyCursor == null ? ' 条' : '+ 条');
    empty.hidden = items.length > 0;
    let emptyText = '心率偏离正常区间后，分析结果会显示在这里。';
    if (data.warning_count > 0) {
      emptyText = isActive(data.state) ? '正在等待预警抽帧或分析结果。' : '本轮预警暂无已保存的抽帧。';
    }
    if (empty.textContent !== emptyText) empty.textContent = emptyText;
    if (signature === eventSignature) return;
    const list = $('ring-events');
    const expanded = new Map([...list.querySelectorAll('details[data-event-id]')]
      .map((event) => [event.dataset.eventId, event.open]));
    const announcements = new Map(current.map((item) => [eventKey(item), item.urgent
      ? '高优先级预警，请立即查看现场' : (verdicts[item.verdict]?.[0] || '等待确认')]));
    const changed = [...announcements].find(([id, message]) => eventAnnouncements.get(id) !== message);
    eventAnnouncements = announcements;
    if (changed) $('ring-event-update').textContent = changed[1] + '。';
    list.replaceChildren(...items.map((item, index) => renderEvent(item, expanded, index === 0)));
    eventSignature = signature;
  }

  async function loadHistory(older = false) {
    if (historyLoading || (older && historyCursor == null)) return;
    historyLoading = true;
    const button = $('ring-history-more');
    const message = $('ring-history-status');
    button.disabled = true;
    message.textContent = '正在读取历史预警…';
    try {
      const endpoint = older ? `history/${historyCursor}` : 'history';
      const data = await request('GET', endpoint, new AbortController());
      historyItems = [...new Map([...historyItems, ...data.events].map((item) => [eventKey(item), item])).values()];
      if (older || historyNeeded) historyCursor = data.next_before;
      historyNeeded = false;
      message.textContent = '';
    } catch {
      message.textContent = '历史记录读取失败，请重试。';
      historyNeeded = true;
    } finally {
      historyLoading = false;
      button.disabled = false;
      button.textContent = historyNeeded ? '重新读取历史' : '加载更早的预警';
      button.hidden = !historyNeeded && historyCursor == null;
      if (status) renderEvents(status);
    }
  }

  function videoLabel(name) {
    if (name === 'video.mp4') return '完整录像';
    const highlight = /^highlight-ring-(\d+)\.mp4$/i.exec(name);
    if (highlight) return '心率预警片段 ' + Number(highlight[1]);
    const segment = /^(?:segments\/)?segment-(\d+)\.mp4$/i.exec(name);
    if (segment) return '录像分段 ' + Number(segment[1]);
    return name;
  }

  function resetVideo() {
    selectedVideo = '';
    $('ring-player').hidden = true;
    $('ring-video-error').hidden = true;
    if (video.hasAttribute('src')) {
      video.pause();
      video.removeAttribute('src');
      video.load();
    }
  }

  function chooseVideo(source, name) {
    if (selectedVideo === source) return;
    selectedVideo = source;
    $('ring-video-name').textContent = videoLabel(name);
    $('ring-video-error').hidden = true;
    $('ring-player').hidden = false;
    video.src = source;
    for (const button of $('ring-videos').querySelectorAll('button')) {
      button.setAttribute('aria-pressed', String(button.dataset.url === source));
    }
    video.play().catch(() => {});
  }

  video.addEventListener('error', () => {
    const error = $('ring-video-error');
    error.textContent = selectedVideo ? '无法播放这段录像，请检查录像是否已经保存。' : '';
    error.hidden = !selectedVideo;
  });

  function renderVideos(data) {
    const items = Array.isArray(data.videos) ? data.videos : [];
    const signature = JSON.stringify(items);
    if (signature === videoSignature) return;
    videoSignature = signature;
    const rows = [];
    for (const item of items) {
      const source = mediaUrl(item.url);
      if (!source) continue;
      const name = String(item.name || '录像');
      const button = element('button', 'ring-video-button');
      button.type = 'button';
      button.dataset.url = source;
      button.setAttribute('aria-pressed', String(selectedVideo === source));
      button.append(element('span', '', videoLabel(name)), element('span', 'ring-video-action', '查看'));
      button.addEventListener('click', () => chooseVideo(source, name));
      const row = element('li');
      row.append(button);
      rows.push(row);
    }
    $('ring-video-count').textContent = rows.length + ' 段';
    $('ring-videos-empty').hidden = rows.length > 0;
    $('ring-videos').replaceChildren(...rows);
  }

  function renderStatus(data) {
    const finished = isActive(status?.state) && !isActive(data.state);
    if (session !== data.session) {
      if (session && !historyNeeded) loadHistory();
      session = data.session;
      resetVideo();
      eventSignature = '';
      eventAnnouncements = new Map();
      $('ring-event-update').textContent = '';
      videoSignature = '';
    }
    status = data;
    renderState(data.state);
    const active = isActive(data.state);
    const demoBadge = $('ring-demo-badge');
    demoBadge.hidden = !data.demo || !(data.session || active);
    demoBadge.textContent = data.analyze_demo ? '模拟心率 · DeepSeek' : '模拟心率';
    if (active) {
      demo.checked = data.demo === true;
      analyzeDemo.checked = data.demo === true && data.analyze_demo === true;
    }
    const hasReading = Number.isFinite(data.bpm) && data.bpm > 0;
    $('ring-bpm').textContent = hasReading ? String(Math.round(data.bpm)) : '—';
    $('ring-pulse-label').textContent = isActive(data.state) ? (
      pulseLabels[data.pulse_state] || (hasReading ? '正在接收心率' : '等待心率读数')
    ) : (hasReading ? '最后一次读数' : '等待心率读数');
    $('ring-baseline-value').textContent = data.baseline == null && isActive(data.state) && hasReading
      ? '标定中' : formatBaseline(data.baseline);
    $('ring-warning-count').textContent = String(data.warning_count ?? 0);
    renderDiagnostic(data);
    renderPreview(data);
    renderEvents(data);
    if (historyNeeded && !historyLoading && !$('ring-history-status').textContent) loadHistory();
    else if (finished && !historyNeeded) loadHistory();
    renderVideos(data);
    updateControls();
  }

  async function request(method, endpoint, controller, payload) {
    const timeout = setTimeout(() => controller.abort(), method === 'GET' ? 5000 : 15000);
    try {
      const options = { method, cache: 'no-store', signal: controller.signal };
      if (payload) {
        options.headers = { 'Content-Type': 'application/json' };
        options.body = JSON.stringify(payload);
      }
      const response = await fetch('/api/ring/' + endpoint, options);
      const data = await response.json().catch(() => null);
      if (!response.ok) throw new Error(data?.error || data?.message || '戒指服务请求失败。');
      if (endpoint === 'test-alarm' || endpoint.startsWith('history')) return data;
      if (!data || !Object.hasOwn(stateLabels, data.state)) throw new Error('无法读取戒指服务状态。');
      return data;
    } finally {
      clearTimeout(timeout);
    }
  }

  function cancelPoll() {
    clearTimeout(pollTimer);
    pollController?.abort();
    pollController = null;
  }

  function schedulePoll(delay) {
    clearTimeout(pollTimer);
    if (!panel.hidden && !document.hidden && !actionPending && !pollController) {
      pollTimer = setTimeout(pollStatus, delay);
    }
  }

  async function pollStatus() {
    if (panel.hidden || document.hidden || actionPending || pollController) return;
    const controller = new AbortController();
    pollController = controller;
    try {
      const data = await request('GET', 'status', controller);
      if (pollController !== controller) return;
      connectionError = '';
      renderStatus(data);
    } catch {
      if (pollController !== controller) return;
      connectionError = '无法连接戒指服务。请用 start.cmd 启动本地页面，服务恢复后会自动刷新。';
      renderState('error', '连接异常');
      renderDiagnostic(status);
      renderPreview(null);
      updateControls();
    } finally {
      if (pollController === controller) {
        pollController = null;
        schedulePoll(isActive(status?.state) ? 1600 : 3000);
      }
    }
  }

  async function performAction(endpoint, payload) {
    if (actionPending) return;
    cancelPoll();
    actionPending = true;
    actionError = '';
    $('ring-test-alarm-result').hidden = true;
    updateControls();
    try {
      const data = await request('POST', endpoint, new AbortController(), payload);
      connectionError = '';
      renderStatus(data);
    } catch (error) {
      actionError = error.name === 'AbortError' ? '戒指服务响应超时，正在刷新当前状态。' : error.message;
    } finally {
      actionPending = false;
      updateControls();
      schedulePoll(0);
    }
  }

  async function testAlarm() {
    if ($('ring-test-alarm').disabled) return;
    testingAlarm = true;
    const result = $('ring-test-alarm-result');
    result.hidden = false;
    setDiagnostic('ring-test-alarm-result', ['正在试听警报声…', 'warning']);
    updateControls();
    try {
      const data = await request('POST', 'test-alarm', new AbortController(), {});
      if (data?.played !== true) throw new Error('本机服务没有确认试听结果。');
      setDiagnostic('ring-test-alarm-result', ['Windows 已完成试听。请确认声音是否从音箱发出。', 'success']);
    } catch (error) {
      const message = error.name === 'AbortError' ? '试听响应超时，未确认播放结果。' : error.message;
      setDiagnostic('ring-test-alarm-result', [message, 'error']);
    } finally {
      testingAlarm = false;
      updateControls();
    }
  }

  function normalizeBaseline(value) {
    const match = /^(\d{1,3}(?:\.\d{1,2})?)(?:\s*[:：\-–—~～]\s*(\d{1,3}(?:\.\d{1,2})?))?$/.exec(value.trim());
    if (!match) return '';
    const lower = Number(match[1]);
    const upper = match[2] == null ? lower : Number(match[2]);
    if (lower < 35 || upper > 220 || upper < lower) return '';
    return match[2] ? lower + ':' + upper : String(lower);
  }

  form.addEventListener('submit', (event) => {
    event.preventDefault();
    const baseline = baselineMode.value === 'manual' ? normalizeBaseline(baselineInput.value) : 'auto';
    if (!baseline) {
      baselineInput.setCustomValidity('请输入 35–220 BPM 之间的数值或从低到高的范围，例如 65:85。');
      baselineInput.reportValidity();
      return;
    }
    performAction('start', { baseline, demo: demo.checked, analyze_demo: demo.checked && analyzeDemo.checked });
  });
  baselineMode.addEventListener('change', () => {
    baselineInput.setCustomValidity('');
    updateControls();
    if (baselineMode.value === 'manual') baselineInput.focus();
  });
  baselineInput.addEventListener('input', () => baselineInput.setCustomValidity(''));
  demo.addEventListener('change', () => {
    if (!demo.checked) analyzeDemo.checked = false;
    updateControls();
  });
  analyzeDemo.addEventListener('change', updateControls);
  $('ring-stop').addEventListener('click', () => performAction('stop', {}));
  $('ring-test-alarm').addEventListener('click', testAlarm);
  $('ring-history-more').addEventListener('click', () => loadHistory(!historyNeeded));
  $('ring-view-events').addEventListener('click', () => {
    const heading = $('ring-events-title');
    heading.focus({ preventScroll: true });
    heading.scrollIntoView({ block: 'start' });
  });

  const ringTabs = [$('ring-nav'), $('mobile-ring-nav')];
  const voiceTabs = [$('workspace-nav'), $('mobile-workspace-nav')];
  const ringHash = '#ring-monitor';

  function selectTabs(tabs, selected) {
    for (const tab of tabs) {
      tab.classList.toggle('active', selected);
      if (selected) tab.setAttribute('aria-current', 'page');
      else tab.removeAttribute('aria-current');
    }
  }

  function showView(ring, focusTitle = false) {
    if (ring && panel.hidden) document.dispatchEvent(new Event('connect:ring-opening'));
    if (!ring && !panel.hidden) video.pause();
    panel.hidden = !ring;
    $('conversation-view').hidden = ring;
    $('connection-label').hidden = ring;
    $('footer-status').hidden = ring;
    $('workspace-breadcrumb').textContent = ring ? '戒指预警' : '对话工作台';
    selectTabs(ringTabs, ring);
    selectTabs(voiceTabs, !ring);
    if (ring) {
      schedulePoll(0);
      if (focusTitle) $('ring-title').focus({ preventScroll: true });
    } else {
      cancelPoll();
      renderPreview(null);
    }
  }

  function navigate(ring) {
    if (ring !== (location.hash === ringHash)) {
      history.pushState(null, '', ring ? ringHash : location.pathname + location.search);
    }
    showView(ring, true);
    window.scrollTo({ top: 0 });
  }

  for (const tab of ringTabs) tab.addEventListener('click', () => navigate(true));
  for (const tab of voiceTabs) tab.addEventListener('click', () => navigate(false));
  document.addEventListener('click', (event) => {
    if (!panel.hidden && event.target.closest?.('#new-session, #mobile-new, .recent-item, .history-open')) navigate(false);
  }, { capture: true });
  window.addEventListener('hashchange', () => showView(location.hash === ringHash));
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) {
      cancelPoll();
      renderPreview(null);
    } else schedulePoll(0);
  });
  showView(location.hash === ringHash);
})();
