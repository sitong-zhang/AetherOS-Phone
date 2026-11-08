// AetherOS Phone App — 极简 Service Worker：缓存应用外壳，动态接口(/api, /ws)走网络。
const CACHE = "aether-shell-v1";
const FILES = ["/", "/index.html", "/app.js", "/manifest.webmanifest",
               "/icon-192.png", "/icon-512.png"];

self.addEventListener("install", (e) => {
  e.waitUntil(caches.open(CACHE).then((c) => c.addAll(FILES)).catch(() => {}));
  self.skipWaiting();
});
self.addEventListener("activate", (e) => {
  e.waitUntil(self.clients.claim());
});
self.addEventListener("fetch", (e) => {
  const u = e.request.url;
  if (u.includes("/api/") || u.includes("/ws/")) return; // 动态接口不缓存
  e.respondWith(
    caches.match(e.request).then((r) =>
      r || fetch(e.request).then((resp) => {
        const cp = resp.clone();
        caches.open(CACHE).then((c) => c.put(e.request, cp));
        return resp;
      }).catch(() => caches.match("/"))
    )
  );
});
