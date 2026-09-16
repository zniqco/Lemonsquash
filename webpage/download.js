(() => {
  'use strict';

  const link = document.querySelector('#download-link');
  const meta = document.querySelector('.download-meta');
  if (!link) return;

  let downloadReady = false;
  link.addEventListener('click', event => {
    if (!downloadReady) event.preventDefault();
  });

  async function loadLatestRelease() {
    let timeout;
    try {
      const official = new URL('https://lemonsquash.zniq.co/');
      const versionFile = new URL('./updates/latest.txt', document.baseURI);
      const controller = new AbortController();
      timeout = window.setTimeout(() => controller.abort(), 5000);
      const response = await fetch(versionFile, {
        cache: 'no-store', signal: controller.signal, credentials: 'omit',
      });
      if (!response.ok) return;
      const parts = (await response.text()).replace(/^\uFEFF/, '').trim().split('|');
      if (parts.length !== 2) return;
      const [version, displayVersion] = parts.map(part => part.trim());
      link.href = new URL(`updates/${version}.zip`, official).href;
      link.download = `Lemonsquash-${displayVersion}.zip`;
      if (meta) meta.prepend(`v${displayVersion} · `);
      downloadReady = true;
    } catch {
    } finally {
      window.clearTimeout(timeout);
    }
  }

  return loadLatestRelease();
})();
