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
  const eventDialog = $('ring-event-dialog');
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
  let historyItems = [];
  let historyCursor = null;
  let historyLoading = false;
  let historyNeeded = true;
  let historyRetryOlder = false;
  let videoSignature = '';
  let selectedVideo = '';
  let selectedEvent = null;
  let detailController;
  let detailSignature = '';

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
    let alarm = alarmLabels[item.alarm_state] || alarmLabels.unknown;
    if (item.alarm_played) alarm = alarmLabels.played;
    if (alarm === alarmLabels.skipped && Object.hasOwn(alarmSkipLabels, item.alarm_reason)) {
      alarm = [alarmSkipLabels[item.alarm_reason]];
    }
    return { alarm, voice: voiceLabels[item.voice_state] || ['未开始'] };
  }

  function renderDiagnostic(data) {
    const latest = data?.events?.[0];
    const triggered = data?.warning_count > 0;
    const newEvent = triggered && (!latest || data.warning_count > episodeNumber(latest));
    const stages = ['waiting', 'waiting', 'waiting', 'waiting'];
    let message = '等待心率预警';
    let tone = '';
    if (newEvent) {
      const interrupted = data.capture_error || !isActive(data.state);
      stages[0] = interrupted ? 'error' : 'active';
      message = data.capture_error || (interrupted ? '监测结束，未生成抽帧' : '正在收集预警画面');
      tone = interrupted ? 'error' : '';
    } else if (latest) {
      const nextAnalysis = latest.batches?.at(-1)?.analysis_state;
      const analysis = ['queued', 'requesting'].includes(nextAnalysis) ? nextAnalysis : latest.analysis_state;
      stages[0] = latest.capture_state === 'ready' ? 'done' : 'error';
      stages[1] = { queued: 'active', requesting: 'active', received: 'done',
        failed: 'error', skipped: 'skipped' }[analysis] || 'waiting';
      stages[2] = { prompting: 'active', listening: 'active', transcribing: 'active',
        acknowledged: 'done', no_reply: 'done', unconfirmed: 'error', unavailable: 'error',
        stopped: 'skipped' }[latest.voice_state] || 'waiting';
      stages[3] = { playing: 'active', played: 'done', failed: 'error',
        skipped: 'skipped' }[latest.alarm_state] || 'waiting';
      if (stages[3] === 'skipped' && stages[2] === 'waiting') stages[2] = 'skipped';
      const verdict = verdicts[latest.verdict]?.[0];
      if (latest.urgent) {
        message = '高优先级预警 · 请立即查看现场';
        tone = 'error';
      } else if (stages[2] === 'active') {
        message = '正在等待语音回应';
      } else if (stages[1] === 'active') {
        message = '正在分析预警画面';
      } else {
        message = verdict || '正在处理预警';
      }
    } else if (data?.state === 'running' && ['auto', 'CALIBRATING'].includes(data.baseline)) {
      message = '正在校准心率基线';
    }
    if (connectionError) {
      message = '监测服务连接中断';
      tone = 'error';
    }
    const steps = $('ring-progress-stages').children;
    const stepLabels = { waiting: '等待', active: '进行中', done: '完成', skipped: '跳过', error: '异常' };
    let value = 0;
    stages.forEach((state, index) => {
      const step = steps[index];
      step.dataset.state = state;
      step.setAttribute('aria-label', step.textContent + ' · ' + stepLabels[state]);
      step.title = stepLabels[state];
      if (state === 'active') step.setAttribute('aria-current', 'step');
      else step.removeAttribute('aria-current');
      if (state !== 'waiting') value = index + (state === 'active' ? 0.5 : 1);
    });
    $('ring-progress').value = value;
    setDiagnostic('ring-trigger-state', [message, tone]);
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
    if (offset === 0) return '预警时刻';
    return (offset < 0 ? '−' : '+') + (Math.abs(offset) / 1000).toFixed(1) + ' 秒';
  }

  const eventKey = (item) => `${item.session_id || session}/${item.id}`;
  const isTestEvent = (item) => item.demo || item.verdict === 'demo';

  function eventDate(item) {
    const timestamp = new Date(item.created_at);
    const date = Number.isNaN(timestamp.getTime()) ? '心率预警' : timestamp.toLocaleString();
    return isTestEvent(item) ? date + ' · 模拟' : date;
  }

  function eventVerdict(item) {
    return item.urgent ? ['高优先级 · 未收到回应', 'danger']
      : (verdicts[item.verdict] || ['等待确认', 'pending']);
  }

  function renderEvent(item) {
    const [verdict, tone] = eventVerdict(item);
    const button = element('button', 'ring-event');
    button.type = 'button';
    button.dataset.eventId = eventKey(item);
    button.setAttribute('aria-haspopup', 'dialog');
    button.append(
      element('span', 'ring-event-name', eventDate(item)),
      element('span', 'ring-badge ring-verdict ' + tone, verdict),
      element('span', 'ring-event-arrow', '›'),
    );
    button.lastElementChild.setAttribute('aria-hidden', 'true');
    button.addEventListener('click', () => openEvent(item));
    return button;
  }

  function renderEventDetail(item) {
    const [verdict, tone] = eventVerdict(item);
    const badge = $('ring-event-verdict');
    badge.className = 'ring-badge ring-verdict ' + tone;
    badge.textContent = verdict;
    badge.hidden = false;
    const body = $('ring-event-detail');
    body.replaceChildren();
    appendEventResult(body, item);
    const pivot = Number.isFinite(item.onset_ms) ? item.onset_ms : null;
    const phases = ['before', 'after'].map((phase) => {
      const block = element('section', 'ring-phase');
      block.dataset.phase = phase;
      const heading = element('div', 'ring-phase-heading');
      const count = element('span', 'ring-phase-count');
      heading.append(element('h4', 'ring-phase-title', phase === 'before' ? '预警前' : '预警后'), count);
      block.append(heading);
      const notes = item[phase] || [];
      if (notes.length) block.append(element('p', 'ring-phase-observation', notes.join(' ')));
      const frames = element('div', 'ring-frames');
      block.append(frames);
      body.append(block);
      return { frames, count };
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
      phases[before ? 0 : 1].frames.append(link);
    }
    for (const { frames, count } of phases) {
      count.textContent = frames.childElementCount + ' 张';
      if (!frames.childElementCount) frames.append(element('p', 'ring-phase-observation', '这一阶段没有保存抽帧。'));
    }
  }

  function appendEventResult(body, item) {
    const result = element('div', 'ring-event-result');
    if (item.summary) {
      result.append(element('h4', 'ring-result-label', '分析结论'), element('p', 'ring-event-summary', item.summary));
    }
    const { alarm, voice } = eventStages(item);
    const stages = element('dl', 'ring-event-stages');
    for (const [label, value] of [['语音确认', voice], ['警报声', alarm]]) {
      const row = element('div', 'ring-event-stage');
      const status = element('dd', '', value[0]);
      if (value[1]) status.dataset.tone = value[1];
      row.append(element('dt', '', label), status);
      stages.append(row);
    }
    result.append(stages);
    body.append(result);
  }

  function renderEvents(data) {
    const current = Array.isArray(data?.events) ? data.events : [];
    const combined = new Map(historyItems.map((item) => [eventKey(item), item]));
    for (const item of current) combined.set(eventKey(item), item);
    const items = [...combined.values()]
      .sort((a, b) => String(b.created_at).localeCompare(String(a.created_at)));
    const signature = JSON.stringify(items.map((item) => [eventKey(item), eventDate(item), item.verdict, item.urgent]));
    const incomplete = historyNeeded || historyCursor != null;
    $('ring-event-count').textContent = incomplete && !items.length ? '—' : items.length + (incomplete ? '+ 条' : ' 条');
    $('ring-events-empty').hidden = items.length > 0 || historyNeeded;
    if (signature === eventSignature) return;
    const list = $('ring-events');
    const focused = document.activeElement?.dataset.eventId;
    const rows = items.map(renderEvent);
    list.replaceChildren(...rows);
    if (focused) rows.find((row) => row.dataset.eventId === focused)?.focus({ preventScroll: true });
    eventSignature = signature;
  }

  async function readHistory(before) {
    const suffix = before == null ? '' : '/' + before;
    const get = (scope) => request('GET', scope + suffix, new AbortController());
    try {
      return await get('all-history');
    } catch (error) {
      if (error.status !== 404) throw error;
    }
    const [regular, tests] = await Promise.all([
      get('history'),
      get('test-history').catch((error) => {
        if (error.status !== 404) throw error;
        return { events: [], next_before: null };
      }),
    ]);
    // Each old endpoint paginates independently. Advance only as far as both have covered.
    const cursors = [regular.next_before, tests.next_before].filter((cursor) => cursor != null);
    const nextBefore = cursors.length ? Math.max(...cursors) : null;
    const events = [...regular.events, ...tests.events].filter((item) =>
      nextBefore == null || Number(item.session_id?.replace('session-', '')) >= nextBefore);
    return { events, next_before: nextBefore };
  }

  async function loadHistory(older = false) {
    if (historyLoading || (older && historyCursor == null)) return;
    historyLoading = true;
    const button = $('ring-history-more');
    const message = $('ring-history-status');
    button.disabled = true;
    message.textContent = '正在读取历史预警…';
    try {
      const data = await readHistory(older ? historyCursor : null);
      if (!Array.isArray(data?.events) || (data.next_before != null && !Number.isSafeInteger(data.next_before))) {
        throw new Error('预警服务返回的历史记录格式有误。');
      }
      historyItems = [...new Map([...historyItems, ...data.events].map((item) => [eventKey(item), item])).values()];
      if (older || historyNeeded) historyCursor = data.next_before;
      historyNeeded = false;
      historyRetryOlder = false;
      message.textContent = '';
    } catch (error) {
      message.textContent = error.name === 'AbortError' ? '历史记录读取超时，请重试。'
        : error.status === 503 ? error.message : '历史记录读取失败，请重试。';
      historyNeeded = true;
      historyRetryOlder = older;
    } finally {
      historyLoading = false;
      button.disabled = false;
      button.textContent = historyNeeded ? '重新读取历史' : '加载更早的预警';
      button.hidden = !historyNeeded && historyCursor == null;
      renderEvents(status);
    }
  }

  function videoLabel(name) {
    if (name === 'video.mp4') return '本轮完整录像';
    const highlight = /^highlight-ring-(\d+)\.mp4$/i.exec(name);
    if (highlight) return '心率预警片段 ' + Number(highlight[1]);
    const segment = /^(?:segments\/)?segment-(\d+)\.mp4$/i.exec(name);
    if (segment) return '本轮录像分段 ' + Number(segment[1]);
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
    const clips = [];
    const full = [];
    for (const item of items) {
      const source = mediaUrl(item.url);
      if (!source) continue;
      const name = String(item.name || '录像');
      const button = element('button', 'ring-video-button');
      button.type = 'button';
      button.dataset.url = source;
      button.setAttribute('aria-pressed', String(selectedVideo === source));
      button.append(element('span', '', videoLabel(name)), element('span', 'ring-video-action', '播放'));
      button.addEventListener('click', () => chooseVideo(source, name));
      const row = element('li');
      row.append(button);
      (/^highlight-ring-\d+\.mp4$/i.test(name) ? clips : full).push(row);
    }
    $('ring-video-count').textContent = (clips.length + full.length) + ' 段';
    $('ring-clips-empty').hidden = clips.length > 0;
    $('ring-videos-empty').hidden = full.length > 0;
    $('ring-clips-list').replaceChildren(...clips);
    $('ring-full-list').replaceChildren(...full);
  }

  async function readEvent(key, controller) {
    try {
      return await request('GET', 'all-event/' + key, controller);
    } catch (error) {
      if (error.status !== 404) throw error;
    }
    const event = status?.events?.find((item) => eventKey(item) === key)
      || historyItems.find((item) => eventKey(item) === key);
    if (!event) throw new Error('找不到这条预警记录。');
    try {
      return await request('GET', (isTestEvent(event) ? 'test-event/' : 'event/') + key, controller);
    } catch (error) {
      if (error.status !== 404) throw error;
      const videos = key.startsWith(session + '/') ? (status?.videos || []).filter((item) => item.name === 'video.mp4') : [];
      return { event, videos };
    }
  }

  async function refreshEvent() {
    if (!selectedEvent || detailController) return;
    const controller = new AbortController();
    detailController = controller;
    const key = selectedEvent;
    const message = $('ring-event-status');
    try {
      const data = await readEvent(key, controller);
      if (detailController !== controller || selectedEvent !== key) return;
      const signature = JSON.stringify(data.event);
      if (signature !== detailSignature) {
        renderEventDetail(data.event);
        detailSignature = signature;
      }
      renderVideos(data);
      const saving = key.startsWith(session + '/') && isActive(status?.state);
      $('ring-videos-empty').textContent = saving ? '本轮录像仍在保存。' : '暂无可回看的本轮录像。';
      $('ring-clips-empty').textContent = saving ? '预警片段仍在保存。' : '暂无可回看的预警片段。';
      $('ring-detail-grid').hidden = false;
      message.textContent = '';
    } catch (error) {
      if (detailController === controller && selectedEvent === key) {
        message.textContent = error.name === 'AbortError' ? '读取预警详情超时。' : error.message;
      }
    } finally {
      if (detailController === controller) detailController = null;
    }
  }

  function openEvent(item) {
    selectedEvent = eventKey(item);
    detailSignature = '';
    videoSignature = '';
    $('ring-event-title').textContent = eventDate(item);
    $('ring-event-status').textContent = '正在读取预警详情…';
    $('ring-event-verdict').hidden = true;
    $('ring-detail-grid').hidden = true;
    eventDialog.showModal();
    eventDialog.scrollTop = 0;
    refreshEvent();
  }

  $('ring-event-back').addEventListener('click', () => eventDialog.close());
  eventDialog.addEventListener('close', () => {
    const key = selectedEvent;
    selectedEvent = null;
    detailController?.abort();
    detailController = null;
    resetVideo();
    const record = [...$('ring-events').children].find((row) => row.dataset.eventId === key);
    (record || $('ring-events-title')).focus({ preventScroll: true });
  });

  function renderStatus(data) {
    const finished = isActive(status?.state) && !isActive(data.state);
    if (session !== data.session) {
      if (session && !historyNeeded) loadHistory();
      session = data.session;
      eventSignature = '';
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
    if (selectedEvent && selectedEvent.startsWith(session + '/')) refreshEvent();
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
      if (!response.ok) {
        const error = new Error(data?.error || data?.message || '戒指服务请求失败。');
        error.status = response.status;
        throw error;
      }
      if (endpoint === 'test-alarm' || /^(?:(?:all-|test-)?history(?:\/|$)|(?:all-|test-)?event\/)/.test(endpoint)) return data;
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
      result.hidden = true;
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
  $('ring-history-more').addEventListener('click', () => loadHistory(historyRetryOlder || !historyNeeded));
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
    if (!ring && eventDialog.open) eventDialog.close();
    panel.hidden = !ring;
    $('conversation-view').hidden = ring;
    $('connection-label').hidden = ring;
    $('footer-status').hidden = ring;
    $('workspace-breadcrumb').textContent = ring ? '戒指预警' : '对话工作台';
    selectTabs(ringTabs, ring);
    selectTabs(voiceTabs, !ring);
    if (ring) {
      if (historyNeeded && !$('ring-history-status').textContent) loadHistory();
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
  window.addEventListener('hashchange', () => showView(location.hash === ringHash));
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) {
      cancelPoll();
      renderPreview(null);
    } else schedulePoll(0);
  });
  showView(location.hash === ringHash);
})();
