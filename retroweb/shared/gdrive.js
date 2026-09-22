"use strict";
// Shared Google Drive helpers: GIS OAuth token client + Drive REST download,
// plus (for arcade ROM loading) the Google Picker. IBM PC/AT's HDD sync and
// the arcade "Load ROM from Google Drive" button both talk to this object.
//
// GDRIVE_CLIENT_ID is a real, working OAuth Client ID for this site's own
// deployment (see IBM_PCAT_REVIEW.md §39) -- scoped to Authorized JavaScript
// origins in Cloud Console, so it simply won't authenticate from anywhere
// else. A fork serving this from a different origin needs its own (Client
// IDs are public identifiers, safe to commit -- unlike a client secret,
// which this flow never uses). GDRIVE_API_KEY is required only by the
// Picker (arcade ROM loading); IBM's named-file HDD sync does not need it.
// Both are public browser credentials (referrer / origin restricted). Callers
// still report "not set up yet" when either is a REPLACE_ME placeholder, or
// under `?test=1&gdrive_unconfigured=1`.
(function (global) {
  const GDRIVE_CLIENT_ID = "610038606273-nmjm5il8en76ge8i6heh908073b1klh0.apps.googleusercontent.com";
  const GDRIVE_API_KEY = "AIzaSyAw-UToUaukgpCErUrzpWzOVskHkD4eBR4";
  // Cloud project number -- Picker's setAppId. Without it, drive.file lets
  // you pick a file (including one inside a folder) but Drive then 404s
  // the download because the grant never bound to this app.
  const GDRIVE_APP_ID = GDRIVE_CLIENT_ID.split("-")[0];
  const GDRIVE_SCOPE = "https://www.googleapis.com/auth/drive.file";
  const FOLDER_MIME = "application/vnd.google-apps.folder";
  const ROM_DRIVE_ACK_KEY = "retroweb.romDriveLegalAck";

  let tokenClient = null;
  let accessToken = null;   // in-memory only -- never persisted anywhere
  let tokenExpiry = 0;      // ms epoch
  let pickerReady = false;
  // Token promise started from the Drive/OK click, before any await.
  let pendingGestureToken = null;

  function testMode() {
    try {
      return new URLSearchParams(location.search).get("test") === "1";
    } catch {
      return false;
    }
  }

  function testForceUnconfigured() {
    try {
      const p = new URLSearchParams(location.search);
      return p.get("test") === "1" && p.get("gdrive_unconfigured") === "1";
    } catch {
      return false;
    }
  }

  // `?test=1&gdrive_unconfigured=1` forces the not-configured code path
  // regardless of GDRIVE_CLIENT_ID's real value -- the only way Playwright
  // can deterministically exercise that path without a real Google sign-in.
  // Gated behind test=1 like every other test-only override, so no real
  // visitor's URL can reach it.
  function isClientConfigured() {
    return !testForceUnconfigured() && !GDRIVE_CLIENT_ID.startsWith("REPLACE_ME");
  }
  function isPickerConfigured() {
    return isClientConfigured() && !GDRIVE_API_KEY.startsWith("REPLACE_ME");
  }

  function gisReady() {
    return !!(global.google && google.accounts && google.accounts.oauth2);
  }

  // Live GIS login is skipped under `?test=1` so Playwright never pops a
  // real Google window. `__gdriveAllowTokenInTest` is a Playwright-only
  // hook: the suite stubs `google.accounts.oauth2` and sets the flag to
  // assert the OK/Drive click still starts `requestAccessToken`.
  function allowLiveGoogle() {
    if (!isClientConfigured()) return false;
    if (!testMode()) return true;
    try { return !!global.__gdriveAllowTokenInTest; } catch { return false; }
  }

  function waitForGis(timeoutMs) {
    const ms = timeoutMs == null ? 5000 : timeoutMs;
    return new Promise((resolve, reject) => {
      const start = Date.now();
      (function poll() {
        if (gisReady()) { resolve(); return; }
        if (Date.now() - start > ms) { reject(new Error("Google sign-in script failed to load")); return; }
        setTimeout(poll, 100);
      })();
    });
  }

  function ensureTokenClient() {
    if (tokenClient) return;
    tokenClient = google.accounts.oauth2.initTokenClient({
      client_id: GDRIVE_CLIENT_ID,
      scope: GDRIVE_SCOPE,
      // FedCM lets a returning visitor's silent (prompt: "") token
      // request complete via the browser's own native account-chooser
      // mediation instead of a Google popup window. GIS's classic
      // silent flow still opens an actual popup under the hood even
      // with prompt: "" -- it just skips the account-picker/consent
      // screens *inside* that window when there's an active Google
      // session and prior grant -- and some browsers' popup blockers
      // can swallow that popup outright since it isn't fired from a
      // direct click. FedCM support varies by browser (Chrome/Edge
      // yes; Safari/Firefox not yet); where it isn't supported, GIS
      // falls back to the normal popup-based flow.
      use_fedcm_for_auth: true,
      callback: () => {},  // replaced per-request below; initTokenClient requires one up front
    });
  }

  // Must run in the same turn as the click (Chrome drops user activation
  // across await). prompt: "consent" forces a GIS popup whose window.closed
  // poll is blocked by Google's COOP header, so the token never returns.
  function requestAccessTokenNow(_interactive) {
    ensureTokenClient();
    return new Promise((resolve, reject) => {
      tokenClient.callback = (resp) => {
        if (resp && resp.error) { reject(new Error(resp.error)); return; }
        accessToken = resp.access_token;
        tokenExpiry = Date.now() + resp.expires_in * 1000 - 30_000;
        resolve(accessToken);
      };
      tokenClient.error_callback = (err) => {
        reject(new Error((err && err.type) || "FedCM/token request failed"));
      };
      tokenClient.requestAccessToken({ prompt: "" });
    });
  }

  function startInteractiveTokenFromGesture() {
    if (!allowLiveGoogle()) return null;
    if (accessToken && Date.now() < tokenExpiry) return Promise.resolve(accessToken);
    if (!gisReady()) return null;
    return requestAccessTokenNow(true);
  }

  function googleAuthCancelled(err) {
    const msg = (err && err.message) || String(err || "");
    return /popup_closed|popup_failed|access_denied/i.test(msg);
  }

  // Resolves with a valid access token, requesting a fresh one only if the
  // one already held is missing or about to expire. `interactive` allows
  // Google to show a popup/consent screen if a silent refresh isn't
  // possible -- only ever pass true from a real, direct click; a boot-time
  // pull always passes false, so a bare page load never pops a login on
  // its own.
  async function getToken(interactive) {
    if (accessToken && Date.now() < tokenExpiry) return accessToken;
    if (!gisReady()) await waitForGis();
    return requestAccessTokenNow(interactive);
  }

  async function apiFetch(url, opts, token, signal) {
    const res = await fetch(url, {
      ...opts,
      signal,
      headers: { ...(opts && opts.headers), Authorization: "Bearer " + token },
    });
    if (!res.ok) {
      const body = await res.text().catch(() => "");
      throw new Error(`Google Drive API error ${res.status}: ${body.slice(0, 200)}`);
    }
    return res;
  }

  async function download(fileId, token, signal) {
    const res = await apiFetch(
      `https://www.googleapis.com/drive/v3/files/${encodeURIComponent(fileId)}?alt=media&supportsAllDrives=true`,
      { method: "GET" }, token, signal);
    return new Uint8Array(await res.arrayBuffer());
  }

  async function listChildren(folderId, token) {
    const params = new URLSearchParams({
      q: `'${folderId}' in parents and trashed=false`,
      fields: "files(id,name,mimeType)",
      pageSize: "100",
      supportsAllDrives: "true",
      includeItemsFromAllDrives: "true",
    });
    const res = await apiFetch(
      "https://www.googleapis.com/drive/v3/files?" + params.toString(),
      { method: "GET" }, token);
    const data = await res.json();
    return data.files || [];
  }

  function waitForPicker(timeoutMs) {
    const ms = timeoutMs == null ? 8000 : timeoutMs;
    if (pickerReady && global.google && google.picker) return Promise.resolve();
    return new Promise((resolve, reject) => {
      const start = Date.now();
      (function poll() {
        if (global.gapi && gapi.load) {
          gapi.load("picker", {
            callback: () => {
              if (global.google && google.picker) { pickerReady = true; resolve(); }
              else reject(new Error("Google Picker failed to load"));
            },
            onerror: () => reject(new Error("Google Picker failed to load")),
          });
          return;
        }
        if (Date.now() - start > ms) {
          reject(new Error("Google Picker script failed to load"));
          return;
        }
        setTimeout(poll, 100);
      })();
    });
  }

  // Opens the Google Picker so the visitor can grant drive.file access to
  // existing files (a zip, or loose chips). Resolves to an array of
  // {id, name, mimeType}; empty on cancel. Pass `opts.token` when the
  // caller already started GIS from a user gesture; otherwise this
  // requests one (and must itself still be on a click stack).
  async function pickFiles(opts) {
    opts = opts || {};
    const token = opts.token || await getToken(true);
    await waitForPicker();
    return new Promise((resolve) => {
      const view = new google.picker.DocsView(google.picker.ViewId.DOCS)
        .setIncludeFolders(true)
        .setSelectFolderEnabled(true)
        .setMode(google.picker.DocsViewMode.LIST);
      const builder = new google.picker.PickerBuilder()
        .addView(view)
        .setAppId(GDRIVE_APP_ID)
        .setOAuthToken(token)
        .setDeveloperKey(GDRIVE_API_KEY)
        .setTitle(opts.title || "Select ROM files")
        .setCallback((data) => {
          if (data[google.picker.Response.ACTION] === google.picker.Action.PICKED) {
            const docs = data[google.picker.Response.DOCUMENTS] || [];
            resolve(docs.map((d) => ({
              id: d[google.picker.Document.ID],
              name: d[google.picker.Document.NAME],
              mimeType: d[google.picker.Document.MIME_TYPE],
            })));
            return;
          }
          if (data[google.picker.Response.ACTION] === google.picker.Action.CANCEL) {
            resolve([]);
          }
        });
      if (opts.multiSelect !== false) builder.enableFeature(google.picker.Feature.MULTISELECT_ENABLED);
      builder.enableFeature(google.picker.Feature.SUPPORT_DRIVES);
      builder.build().setVisible(true);
    });
  }

  function romDriveAcked() {
    try { return localStorage.getItem(ROM_DRIVE_ACK_KEY) === "1"; } catch { return false; }
  }
  function setRomDriveAcked() {
    try { localStorage.setItem(ROM_DRIVE_ACK_KEY, "1"); } catch {}
  }

  // First click of "Load ROM from Google Drive" shows #romDriveLegalHint
  // once per browser. OK persists the ack and continues; Cancel does not.
  // GIS login starts inside the OK handler so Chrome still treats it as
  // the click (the continuation after this promise is not a user gesture).
  function ensureRomDriveLegal() {
    if (romDriveAcked()) return Promise.resolve(true);
    const dlg = document.getElementById("romDriveLegalHint");
    if (!dlg) return Promise.resolve(false);
    return new Promise((resolve) => {
      const ok = document.getElementById("romDriveLegalOk");
      const cancel = document.getElementById("romDriveLegalCancel");
      const done = (accepted) => {
        ok && ok.removeEventListener("click", onOk);
        cancel && cancel.removeEventListener("click", onCancel);
        dlg.removeEventListener("cancel", onEsc);
        if (dlg.open) dlg.close();
        if (accepted) {
          setRomDriveAcked();
          pendingGestureToken = startInteractiveTokenFromGesture();
        }
        resolve(accepted);
      };
      const onOk = () => done(true);
      const onCancel = () => done(false);
      const onEsc = (e) => { e.preventDefault(); done(false); };
      ok && ok.addEventListener("click", onOk);
      cancel && cancel.addEventListener("click", onCancel);
      dlg.addEventListener("cancel", onEsc);
      if (!dlg.open) dlg.showModal();
    });
  }

  // Arcade entry point: legal dialog (first time), GIS login from the
  // click that proceeds, then Picker, then download. Returns null if the
  // visitor cancelled or Drive isn't set up (setStatus is called for the
  // unconfigured path); [] if they closed the Picker; otherwise
  // [{name, bytes}, ...].
  async function pickAndDownloadRoms(opts) {
    opts = opts || {};
    const setStatus = opts.setStatus || (() => {});
    pendingGestureToken = null;

    if (romDriveAcked()) {
      pendingGestureToken = startInteractiveTokenFromGesture();
    } else if (!(await ensureRomDriveLegal())) {
      return null;
    }

    if (!isClientConfigured() || (testMode() && !allowLiveGoogle())) {
      setStatus("Not set up yet — Google Drive ROM loading needs a Picker API key.");
      return null;
    }

    setStatus("Opening Google Drive…");
    let token;
    try {
      if (pendingGestureToken) {
        token = await pendingGestureToken;
      } else {
        token = await getToken(true);
      }
    } catch (e) {
      if (googleAuthCancelled(e)) {
        setStatus("");
        return null;
      }
      setStatus("Google Drive: " + ((e && e.message) || e));
      return null;
    } finally {
      pendingGestureToken = null;
    }

    if (!isPickerConfigured()) {
      setStatus("Not set up yet — Google Drive ROM loading needs a Picker API key.");
      return null;
    }

    try {
      const picked = await pickFiles({
        title: opts.title || "Select a ROM zip, chips, or the folder that holds them",
        token,
      });
      if (!picked.length) return [];
      const out = [];
      for (const doc of picked) {
        const toGet = [];
        if (doc.mimeType === FOLDER_MIME) {
          const kids = await listChildren(doc.id, token);
          for (const k of kids) {
            if (k.mimeType !== FOLDER_MIME) toGet.push(k);
          }
          if (!toGet.length) {
            throw new Error("That folder is empty — open it and select the ROM zip or chips.");
          }
        } else {
          toGet.push(doc);
        }
        for (const f of toGet) {
          try {
            out.push({ name: f.name, bytes: await download(f.id, token) });
          } catch (e) {
            const msg = (e && e.message) || "";
            if (/404|File not found/.test(msg)) {
              throw new Error("Drive couldn't open \"" + f.name + "\". Open the folder and select the ROM zip or chips inside it.", { cause: e });
            }
            throw e;
          }
        }
      }
      return out;
    } catch (e) {
      setStatus("Google Drive: " + ((e && e.message) || e));
      return null;
    }
  }

  global.RetroGdrive = {
    CLIENT_ID: GDRIVE_CLIENT_ID,
    API_KEY: GDRIVE_API_KEY,
    APP_ID: GDRIVE_APP_ID,
    SCOPE: GDRIVE_SCOPE,
    isClientConfigured,
    isPickerConfigured,
    waitForGis,
    getToken,
    apiFetch,
    download,
    waitForPicker,
    pickFiles,
    romDriveAcked,
    ensureRomDriveLegal,
    pickAndDownloadRoms,
  };
})(typeof window !== "undefined" ? window : globalThis);
