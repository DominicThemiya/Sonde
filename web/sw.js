// keeps a copy of the page so the installed app still opens when the
// server can't be reached. songs, covers and the song list always go
// straight to the network, so streaming and seeking aren't touched

const CACHE = "sonde-shell";

self.addEventListener("install", (event) => {
  event.waitUntil(caches.open(CACHE).then((cache) => cache.add("/")));
  self.skipWaiting();
});

self.addEventListener("activate", (event) => {
  event.waitUntil(self.clients.claim());
});

self.addEventListener("fetch", (event) => {
  if (event.request.mode !== "navigate") {
    return;
  }

  // newest page when the server answers, the saved one when it doesn't
  event.respondWith(
    fetch(event.request)
      .then((response) => {
        if (response.ok) {
          const copy = response.clone();
          caches.open(CACHE).then((cache) => cache.put("/", copy));
        }
        return response;
      })
      .catch(() => caches.match("/")),
  );
});
