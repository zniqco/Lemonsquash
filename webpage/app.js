(() => {
  'use strict';

  const typed = document.querySelector('#typed-text');
  const results = document.querySelector('#launcher-results');
  const launcher = document.querySelector('.launcher');
  if (!typed || !results || !launcher) return;
  const motion = window.matchMedia('(prefers-reduced-motion: reduce)');
  const scenes = [
    {
      query: 'lemonsquash', icon: 'google.svg',
      rows: [['lemonsquash', 'Search in Google'], ['lemonsquash windows', 'Search in Google'], ['lemonsquash launcher', 'Search in Google'], ['lemonsquash download', 'Search in Google']],
    },
    {
      query: 'powershell', icon: 'terminal.svg',
      rows: [['PowerShell', ''], ['Windows PowerShell', ''], ['Windows PowerShell ISE', ''], ['Windows PowerShell ISE (x86)', '']],
    },
    {
      query: '128 * 24', icon: 'calculator.svg',
      waitForFullQuery: true,
      rows: [['3072', 'Copy value']],
    },
  ];

  const timing = { typing: 60, deleting: 30, hold: 2500, empty: 500 };
  let sceneIndex = Math.floor(Math.random() * scenes.length);
  let deleting = false;
  let timer;

  function renderRows(scene, ready) {
    results.replaceChildren();
    launcher.classList.toggle('has-results', ready && scene.rows.length > 0);
    if (!ready) return;
    for (const [index, [title, detail]] of scene.rows.entries()) {
      const row = document.createElement('div');
      row.className = `result${index === 0 ? ' selected' : ''}`;
      const icon = document.createElement('img');
      icon.src = `./assets/${scene.icon}`;
      icon.alt = '';
      const copy = document.createElement('div');
      copy.className = 'result-copy';
      const heading = document.createElement('span');
      heading.className = 'result-title';
      heading.textContent = title;
      copy.append(heading);
      if (detail) {
        const subheading = document.createElement('span');
        subheading.className = 'result-description';
        subheading.textContent = detail;
        copy.append(subheading);
      }
      row.append(icon, copy);
      results.append(row);
    }
  }

  function canPlay() {
    return !motion.matches && !document.hidden;
  }

  function schedule(delay) {
    clearTimeout(timer);
    if (canPlay()) timer = window.setTimeout(tick, delay);
  }

  function tick() {
    if (!canPlay()) return;
    const scene = scenes[sceneIndex];
    if (deleting) {
      typed.textContent = typed.textContent.slice(0, -1);
      if (typed.textContent.length < 3) renderRows(scene, false);
      if (!typed.textContent.length) {
        sceneIndex = (sceneIndex + 1) % scenes.length;
        deleting = false;
      }
      schedule(deleting ? timing.deleting : timing.empty);
      return;
    }

    const length = typed.textContent.length + 1;
    typed.textContent = scene.query.slice(0, length);
    const finished = length === scene.query.length;
    renderRows(scene, scene.waitForFullQuery ? finished : length >= 3);
    deleting = finished;
    schedule(finished ? timing.hold : timing.typing);
  }

  document.addEventListener('visibilitychange', () => schedule(timing.empty));
  motion.addEventListener('change', () => {
    if (motion.matches) {
      typed.textContent = scenes[sceneIndex].query;
      renderRows(scenes[sceneIndex], true);
      deleting = true;
    }
    schedule(timing.empty);
  });

  typed.textContent = '';
  renderRows(scenes[sceneIndex], false);
  schedule(timing.empty);
})();
