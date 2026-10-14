const statusLabel = document.getElementById('system-status-label');
const sidebarStatus = document.getElementById('sidebar-status');
const programInput = document.getElementById('program-input');
const sourceInput = document.getElementById('source-input');
const expectedInput = document.getElementById('expected-input');
const feedback = document.getElementById('request-feedback');
const startButtons = [document.getElementById('start-btn'), document.getElementById('start-run-btn')];
const pauseButtons = [document.getElementById('pause-btn'), document.getElementById('pause-run-btn')];
const stopButtons = [document.getElementById('stop-btn'), document.getElementById('stop-run-btn')];
const agentNames = ['Orchestrator', 'Generator', 'Executor', 'Reviewer', 'Tester', 'Monitor'];
let latestStatus = null;
let requestPending = false;

async function fetchJson(url, options) {
  const response = await fetch(url, options);
  const result = await response.json();
  if (!response.ok) {
    throw new Error(result.error || `Request failed (${response.status})`);
  }
  return result;
}

function showView(viewName) {
  document.querySelectorAll('[data-view]').forEach((button) => {
    button.classList.toggle('active', button.dataset.view === viewName);
  });
  document.querySelectorAll('.view-page').forEach((view) => {
    view.classList.toggle('hidden', view.id !== `${viewName}-view`);
  });
  if (window.location.hash !== `#${viewName}`) {
    window.history.replaceState(null, '', `#${viewName}`);
  }
}

function appendText(parent, tag, text, className) {
  const element = document.createElement(tag);
  element.textContent = text == null || text === '' ? '—' : String(text);
  if (className) element.className = className;
  parent.appendChild(element);
  return element;
}

function renderCatalog(programs) {
  const catalog = document.getElementById('program-catalog');
  catalog.replaceChildren();
  programs.forEach((program) => {
    const card = document.createElement('button');
    card.type = 'button';
    card.className = 'catalog-card';
    appendText(card, 'strong', program.name);
    appendText(card, 'span', program.description);
    appendText(card, 'small', program.example, 'catalog-example');
    card.addEventListener('click', () => {
      programInput.value = program.example;
      programInput.focus();
    });
    catalog.appendChild(card);
  });
}

function getAgent(status, name) {
  return (status.agents || []).find((agent) => agent.name === name) || {
    name, role: 'Agent', pid: '—', status: 'IDLE', task: 'Waiting',
    ipcReceived: '—', ipcSent: '—', details: [],
  };
}

function renderAgentWork(status) {
  for (const name of agentNames) {
    const card = document.querySelector(`[data-agent="${name}"]`);
    if (!card) continue;
    const agent = getAgent(status, name);
    const badge = card.querySelector('.agent-status');
    badge.textContent = agent.status || 'IDLE';
    badge.dataset.state = String(agent.status || 'IDLE').toLowerCase();

    const details = card.querySelector('.agent-details');
    details.replaceChildren();
    const rows = [
      ['PID', agent.pid],
      ['Role', agent.role || 'Agent'],
      ['FIFO received', agent.ipcReceived],
      ['FIFO sent', agent.ipcSent],
    ];
    if (agent.memoryKb != null) rows.push(['Memory', `${(agent.memoryKb / 1024).toFixed(1)} MB`]);
    if (agent.cpuTicks != null) rows.push(['CPU ticks', agent.cpuTicks]);
    if (agent.processState) rows.push(['Process state', agent.processState]);
    rows.forEach(([label, value]) => {
      appendText(details, 'dt', label);
      appendText(details, 'dd', value);
    });
    appendText(details, 'dt', 'Current action');
    appendText(details, 'dd', agent.task);

    const detail = card.querySelector('.work-detail');
    detail.replaceChildren();
    (agent.details || []).forEach((line) => appendText(detail, 'p', line));
  }

  const generator = document.querySelector('[data-agent="Generator"] .code-output');
  generator.textContent = status.sourceCode || 'Generated C source will appear here.';

  const executor = document.querySelector('[data-agent="Executor"] .terminal-output');
  executor.textContent = [
    `$ ${status.compilerCommand || 'gcc command pending'}`,
    `Compile: ${status.compileStatus || '—'}`,
    status.compileOutput || '',
    '',
    'Program output:',
    status.programOutput || 'Waiting for Executor...',
  ].filter((line) => line !== '').join('\n');

  const reviewer = document.querySelector('[data-agent="Reviewer"] .work-detail');
  reviewer.replaceChildren();
  appendText(reviewer, 'p', `File lock: ${status.lockState || 'UNLOCKED'}`);
  appendText(reviewer, 'p', `Backup: ${status.backupPath || '—'}`);
  appendText(reviewer, 'p', `Review decision: ${status.reviewResult || 'PENDING'}`);
  (status.reviewDetails || []).forEach((line) => appendText(reviewer, 'p', line));

  const tester = document.querySelector('[data-agent="Tester"] .work-detail');
  tester.replaceChildren();
  appendText(tester, 'p', `Result: ${status.testResult || 'PENDING'}`, 'test-result');
  document.getElementById('expected-display').textContent = status.expectedOutput || '—';
  document.getElementById('actual-display').textContent = status.actualOutput || '—';

  document.getElementById('run-id').textContent = status.currentRunId || '—';
  document.getElementById('run-request').textContent = status.request || 'Waiting for a program';
  document.getElementById('run-status').textContent = status.phase || 'READY';
  document.getElementById('run-monitor').textContent = status.monitorStatus || getAgent(status, 'Monitor').status;
}

function renderLiveFlow(status) {
  document.getElementById('flow-run-id').textContent = status.currentRunId || '—';
  document.getElementById('flow-program').textContent = status.selectedProgram || '—';
  document.getElementById('flow-phase').textContent = status.phase || 'READY';
  document.getElementById('flow-workspace').textContent = status.workspace || '/tmp/aura_runtime_workspace';
  const flow = document.getElementById('live-agent-flow');
  flow.replaceChildren();

  agentNames.slice(0, 5).forEach((name, index) => {
    const agent = getAgent(status, name);
    const card = document.createElement('article');
    card.className = 'live-flow-card';
    const heading = document.createElement('div');
    heading.className = 'live-flow-heading';
    appendText(heading, 'strong', name);
    appendText(heading, 'span', agent.status, 'agent-status');
    card.appendChild(heading);
    appendText(card, 'p', agent.task || 'Waiting');
    appendText(card, 'small', `PID ${agent.pid || '—'} · received ${agent.ipcReceived || '—'}`);
    if (index < 4) {
      appendText(card, 'div', `FIFO · ${agent.ipcSent || 'Waiting for message'}`, 'flow-handoff');
    }
    flow.appendChild(card);
  });
  const monitor = getAgent(status, 'Monitor');
  const monitorCard = document.createElement('article');
  monitorCard.className = 'live-flow-card monitor-flow-card';
  appendText(monitorCard, 'strong', `Monitor · ${monitor.status}`);
  appendText(monitorCard, 'p', monitor.task || 'Observing pipeline');
  appendText(monitorCard, 'small', `PID ${monitor.pid || '—'}`);
  flow.appendChild(monitorCard);

  const eventStream = document.getElementById('event-stream');
  const scrollTop = eventStream.scrollTop;
  eventStream.replaceChildren();
  const events = (status.events || []).slice().reverse();
  document.getElementById('timeline-count').textContent =
    `${events.length} event${events.length === 1 ? '' : 's'} · ${status.phase || 'READY'}`;
  document.getElementById('timeline-clock').textContent =
    new Date().toLocaleTimeString('en-US', { hour: 'numeric', minute: '2-digit', second: '2-digit' });
  if (!events.length) {
    const empty = appendText(eventStream, 'li', 'Start a program to see live agent events and process telemetry.');
    empty.className = 'timeline-empty';
    return;
  }
  events.forEach((event) => {
    const row = document.createElement('li');
    row.className = 'timeline-event';
    appendText(row, 'time', event.timestamp || '');
    appendText(row, 'span', event.component || 'AURA', 'timeline-component');
    const eventName = appendText(row, 'strong', event.event || 'event', 'event-name');
    eventName.dataset.kind = String(event.event || '').toLowerCase();
    const details = String(event.details || '');
    if (/\b(fail|error|reject|timed out)\b/i.test(details)) {
      eventName.dataset.outcome = 'failure';
    } else if (/\b(pass|complete|approved|success)\b/i.test(details)) {
      eventName.dataset.outcome = 'success';
    }
    const content = document.createElement('div');
    content.className = 'timeline-content';
    appendText(content, 'span', details, 'timeline-details');
    if (event.channel) appendText(content, 'code', event.channel, 'timeline-channel');
    row.appendChild(content);
    eventStream.appendChild(row);
  });
  eventStream.scrollTop = scrollTop;
}

function renderOverview(status) {
  const states = {
    Orchestrator: 'overview-orchestrator-state',
    Generator: 'overview-generator-state',
    Executor: 'overview-executor-state',
    Reviewer: 'overview-reviewer-state',
    Tester: 'overview-tester-state',
    Monitor: 'overview-monitor-state',
  };
  Object.entries(states).forEach(([name, id]) => {
    document.getElementById(id).textContent = getAgent(status, name).status || 'IDLE';
  });
}

function updateControls(status) {
  const active = ['STARTING', 'RUNNING', 'GENERATOR', 'EXECUTOR', 'REVIEWER', 'TESTING', 'PAUSED'].includes(status.phase);
  startButtons.forEach((button) => {
    button.disabled = active || requestPending;
  });
  pauseButtons.forEach((button) => {
    button.disabled = !active || requestPending;
    button.textContent = status.phase === 'PAUSED' ? 'Resume' : 'Pause';
  });
  stopButtons.forEach((button) => {
    button.disabled = !active || requestPending;
  });
  const phase = status.phase || 'READY';
  statusLabel.textContent = phase;
  sidebarStatus.textContent = phase === 'COMPLETE' ? 'Last run complete' : phase === 'FAILED' ? 'Run failed' : phase === 'READY' ? 'Runtime ready' : `Runtime ${phase.toLowerCase()}`;
}

function applyStatus(status) {
  latestStatus = status;
  renderOverview(status);
  renderLiveFlow(status);
  renderAgentWork(status);
  updateControls(status);
}

async function refreshStatus() {
  try {
    applyStatus(await fetchJson('/api/status'));
  } catch (error) {
    statusLabel.textContent = 'SERVICE ERROR';
    sidebarStatus.textContent = 'Service unavailable';
    feedback.textContent = error.message;
    feedback.dataset.state = 'error';
  }
}

async function startRun() {
  if (requestPending) return;
  const payload = {
    request: programInput.value.trim(),
    source: sourceInput.value.trim(),
    expectedOutput: expectedInput.value,
  };
  if (!payload.request) {
    showView('agents-work');
    feedback.textContent = 'Enter a program request to start a run.';
    feedback.dataset.state = 'error';
    programInput.focus();
    return;
  }
  requestPending = true;
  updateControls(latestStatus || { phase: 'READY' });
  feedback.textContent = 'Starting Orchestrator and FIFO-connected agents...';
  feedback.dataset.state = 'working';
  try {
    const result = await fetchJson('/api/start', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    });
    showView('agents-work');
    feedback.textContent = `${result.program} started · ${result.runId}`;
    feedback.dataset.state = 'success';
    await refreshStatus();
  } catch (error) {
    feedback.textContent = error.message;
    feedback.dataset.state = 'error';
  } finally {
    requestPending = false;
    if (latestStatus) updateControls(latestStatus);
  }
}

async function togglePause() {
  try {
    const result = await fetchJson('/api/pause', { method: 'POST' });
    feedback.textContent = result.status === 'paused' ? 'All active worker processes paused.' : 'All active worker processes resumed.';
    feedback.dataset.state = 'success';
    await refreshStatus();
  } catch (error) {
    feedback.textContent = error.message;
    feedback.dataset.state = 'error';
  }
}

async function stopRun() {
  try {
    const result = await fetchJson('/api/stop', { method: 'POST' });
    feedback.textContent = result.status === 'stopping' ? 'Stopping the active run...' : result.status;
    feedback.dataset.state = 'working';
    await refreshStatus();
  } catch (error) {
    feedback.textContent = error.message;
    feedback.dataset.state = 'error';
  }
}

document.querySelectorAll('[data-view]').forEach((button) => {
  button.addEventListener('click', () => showView(button.dataset.view));
});
document.querySelectorAll('[data-open-view]').forEach((button) => {
  button.addEventListener('click', () => showView(button.dataset.openView));
});
startButtons.forEach((button) => button.addEventListener('click', startRun));
pauseButtons.forEach((button) => button.addEventListener('click', togglePause));
stopButtons.forEach((button) => button.addEventListener('click', stopRun));
programInput.addEventListener('keydown', (event) => {
  if (event.key === 'Enter') startRun();
});

fetchJson('/api/programs')
  .then((result) => renderCatalog(result.programs))
  .catch((error) => {
    feedback.textContent = error.message;
    feedback.dataset.state = 'error';
  });

const initialView = window.location.hash.replace('#', '');
showView(['overview', 'live-flow', 'agents-work'].includes(initialView) ? initialView : 'overview');
refreshStatus();
window.setInterval(refreshStatus, 750);
