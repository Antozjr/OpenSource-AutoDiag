// Service worker de OpenSource AutoDiag.
//
// Solo cachea el "shell" estático (HTML/manifest/iconos) para que la app
// cargue al instante e instale como PWA, incluso si la red a quien la
// aloja va lenta o falla un momento. No tiene nada que ver con la
// conexión al ESP32 — eso sigue siendo Web Serial, directo navegador↔USB,
// y no pasa por aquí.
const CACHE_NAME = 'autodiag-shell-v1';
const SHELL_FILES = [
  './app.html',
  './sniffer.html',
  './manifest.json',
  './icons/icon-192.png',
  './icons/icon-512.png',
  './icons/icon-maskable-512.png',
];

self.addEventListener('install', (event) => {
  event.waitUntil(
    caches.open(CACHE_NAME).then((cache) => cache.addAll(SHELL_FILES))
  );
  self.skipWaiting();
});

self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches.keys().then((keys) =>
      Promise.all(keys.filter((k) => k !== CACHE_NAME).map((k) => caches.delete(k)))
    )
  );
  self.clients.claim();
});

// stale-while-revalidate: sirve de caché al instante si hay algo guardado,
// y en paralelo intenta refrescarlo desde la red para la próxima vez.
self.addEventListener('fetch', (event) => {
  if (event.request.method !== 'GET') return;

  event.respondWith(
    caches.match(event.request).then((cached) => {
      const network = fetch(event.request)
        .then((resp) => {
          if (resp && resp.status === 200) {
            const copy = resp.clone();
            caches.open(CACHE_NAME).then((cache) => cache.put(event.request, copy));
          }
          return resp;
        })
        .catch(() => cached);
      return cached || network;
    })
  );
});
