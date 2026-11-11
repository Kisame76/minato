// Tells the app what the page sees once the resources the app serves have come (fetch, same origin): a file, a
// missing file's status, the page's cookies, and how a fetch of a host the app does not serve ends.
function tell(message) {
  nativeBridge.postMessage(JSON.stringify(message));
}
var seen = {type: "ready", origin: location.origin, secure: window.isSecureContext, title: document.title};
fetch("data.json").then(function (response) {
  return response.text();
}).then(function (text) {
  seen.data = text.trim();
  return fetch("missing.json");
}).then(function (response) {
  seen.missing = String(response.status);
  seen.cookie = document.cookie;
  return fetch("https://minato-unreachable.invalid/data.json").then(function () {
    return "loaded";
  }, function (error) {
    return "failed " + error.name;
  });
}).then(function (remote) {
  seen.remote = remote;
  tell(seen);
}).catch(function (error) {
  seen.error = error.name;
  tell(seen);
});
