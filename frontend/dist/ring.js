(() => {
  'use strict';

  const $ = (id) => document.getElementById(id);
  const panel = $('ring-monitor');
  const form = $('ring-form');
  const demo = $('ring-demo-input');
  const baselineInput = $('ring-baseline-input');
  const preview = $('ring-preview');
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
    suspected_fall: ['疑似跌倒或昏倒', 'danger'],
    no_visible_fall: ['未发现明显跌倒', 'clear'],
    uncertain: ['画面无法确认', 'pending'],
    demo: ['演示结果', 'pending'],
    already_alerted: ['本次预警已响铃', 'pending'],
    error: ['分析失败', 'danger'],
    cancelled: ['分析已停止', 'pending'],
    pending: ['等待分析', 'pending'],
  };

  let status = null;
  let actionPending = false;
  let connectionError = '';
  let actionError = '';
  let pollTimer;
  let pollController;
  let session = null;
  let previewSource = '';
  let eventSignature = '';
  let eventAnnouncements = new Map();
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
    $('ring-start').disabled = actionPending || isActive(state) || !status || !!connectionError;
    $('ring-stop').disabled = actionPending || !['starting', 'running'].includes(state);
    baselineInput.disabled = actionPending || isActive(state);
    demo.disabled = baselineInput.disabled;
    const message = actionError || connectionError || status?.error || '';
    const messageNode = $('ring-status-message');
    if (messageNode.textContent !== message) messageNode.textContent = message;
    messageNode.dataset.level = message ? 'error' : '';
    messageNode.hidden = !message;
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

  function renderPreview(data) {
    const source = mediaUrl(data.preview_url);
    const stateText = $('ring-preview-state');
    stateText.textContent = source ? (isActive(data.state) ? '定时刷新' : '最后一帧') : '暂无画面';
    if (source === previewSource) return;
    previewSource = source;
    if (source) {
      preview.src = source;
      return;
    }
    preview.removeAttribute('src');
    preview.hidden = true;
    $('ring-preview-empty').hidden = false;
  }

  preview.addEventListener('load', () => {
    preview.hidden = false;
    $('ring-preview-empty').hidden = true;
  });
  preview.addEventListener('error', () => {
    preview.hidden = true;
    $('ring-preview-empty').hidden = false;
    $('ring-preview-state').textContent = '等待下一帧';
  });

  function frameTime(value) {
    if (!Number.isFinite(value)) return '片段画面';
    if (value === 0) return '预警时刻';
    return (value < 0 ? '预警前 ' : '预警后 ') + (Math.abs(value) / 1000).toFixed(1) + ' 秒';
  }

  function renderEvent(item, expanded) {
    const [verdict, tone] = verdicts[item.verdict] || ['等待确认', 'pending'];
    const event = element('details', 'ring-event');
    event.dataset.eventId = String(item.id);
    event.open = expanded;
    const header = element('summary', 'ring-event-heading');
    const id = /^(?:event-)?(\d+)(?:-(\d+))?$/.exec(String(item.id));
    const title = id ? '预警 ' + id[1] + (id[2] ? ' · 第 ' + Number(id[2]) + ' 组' : '') : '心率预警';
    header.append(
      element('span', 'ring-event-name', title),
      element('span', 'ring-badge ring-verdict ' + tone, verdict),
    );
    const body = element('div', 'ring-event-body');
    const timestamp = item.created_at ? new Date(item.created_at) : null;
    if (timestamp && !Number.isNaN(timestamp.getTime())) {
      const time = element('time', 'ring-event-meta', timestamp.toLocaleString());
      time.dateTime = timestamp.toISOString();
      body.append(time);
    }
    body.append(
      element('p', 'ring-event-summary', item.summary || '正在等待画面分析结果。'),
      element('p', 'ring-event-meta', item.alarm_played ? '已播放警报声' : '未播放警报声'),
    );
    const frames = element('div', 'ring-frames');
    for (const frame of item.frames || []) {
      const source = mediaUrl(frame.url);
      if (!source) continue;
      const time = frameTime(frame.time_ms);
      const link = element('a', 'ring-frame');
      link.href = source;
      link.target = '_blank';
      link.rel = 'noopener';
      const image = element('img');
      image.src = source;
      image.alt = '预警抽帧，' + time;
      image.loading = 'lazy';
      link.append(image, element('span', '', time));
      frames.append(link);
    }
    if (frames.childElementCount) body.append(frames);
    event.append(header, body);
    return event;
  }

  function renderEvents(data) {
    const items = Array.isArray(data.events) ? data.events : [];
    const signature = JSON.stringify(items);
    const empty = $('ring-events-empty');
    $('ring-event-count').textContent = items.length + ' 条';
    empty.hidden = items.length > 0;
    let emptyText = '心率偏离正常区间后，分析结果会显示在这里。';
    if (data.warning_count > 0) {
      emptyText = isActive(data.state) ? '正在等待预警抽帧或分析结果。' : '本轮预警暂无已保存的抽帧。';
    }
    if (empty.textContent !== emptyText) empty.textContent = emptyText;
    if (signature === eventSignature) return;
    const list = $('ring-events');
    const expanded = new Map([...list.children].map((event) => [event.dataset.eventId, event.open]));
    const latest = items.find((item) => eventAnnouncements.get(String(item.id)) !== item.verdict);
    eventAnnouncements = new Map(items.map((item) => [String(item.id), item.verdict]));
    if (latest) $('ring-event-update').textContent = '心率预警更新。' + (verdicts[latest.verdict]?.[0] || '等待确认') + '。';
    list.replaceChildren(...items.map((item, index) => renderEvent(item, expanded.get(String(item.id)) ?? index === 0)));
    eventSignature = signature;
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
    if (session !== data.session) {
      session = data.session;
      resetVideo();
      eventSignature = '';
      eventAnnouncements = new Map();
      $('ring-events').replaceChildren();
      $('ring-event-update').textContent = '';
      videoSignature = '';
    }
    status = data;
    renderState(data.state);
    $('ring-demo-badge').hidden = !data.demo || !(data.session || isActive(data.state));
    const hasReading = Number.isFinite(data.bpm) && data.bpm > 0;
    $('ring-bpm').textContent = hasReading ? String(Math.round(data.bpm)) : '—';
    $('ring-pulse-label').textContent = isActive(data.state) ? (
      pulseLabels[data.pulse_state] || (hasReading ? '正在接收心率' : '等待心率读数')
    ) : (hasReading ? '最后一次读数' : '等待心率读数');
    $('ring-baseline-value').textContent = data.baseline == null && isActive(data.state) && hasReading
      ? '标定中' : formatBaseline(data.baseline);
    $('ring-warning-count').textContent = String(data.warning_count ?? 0);
    renderPreview(data);
    renderEvents(data);
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

  function normalizeBaseline(value) {
    const text = value.trim().toLowerCase();
    if (!text || text === 'auto') return 'auto';
    const match = /^(\d{1,3}(?:\.\d{1,2})?)(?:\s*[:：\-–—~～]\s*(\d{1,3}(?:\.\d{1,2})?))?$/.exec(text);
    if (!match) return '';
    const lower = Number(match[1]);
    const upper = match[2] == null ? lower : Number(match[2]);
    if (lower < 35 || upper > 220 || upper < lower) return '';
    return match[2] ? lower + ':' + upper : String(lower);
  }

  form.addEventListener('submit', (event) => {
    event.preventDefault();
    const value = normalizeBaseline(baselineInput.value);
    if (!value) {
      baselineInput.setCustomValidity('请输入 auto、35–220 之间的数值，或从低到高的区间，例如 65:85。');
      baselineInput.reportValidity();
      return;
    }
    performAction('start', { baseline: value, demo: demo.checked });
  });
  baselineInput.addEventListener('input', () => baselineInput.setCustomValidity(''));
  demo.addEventListener('change', () => {
    $('ring-start-help').textContent = demo.checked
      ? '使用模拟心率和真实摄像头画面，抽帧保存在本机。'
      : '连接戒指并启动 BioScope 数据流。预警时会把抽帧发给 DeepSeek。';
  });
  $('ring-stop').addEventListener('click', () => performAction('stop', {}));

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
    if (document.hidden) cancelPoll();
    else schedulePoll(0);
  });
  showView(location.hash === ringHash);
})();
