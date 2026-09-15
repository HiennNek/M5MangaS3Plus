#include "wifi_server.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "thumb.h"

static const char *TAG = "wifi";
static httpd_handle_t s_server = nullptr;
static bool running = false;
static std::string apSSID = "M5Manga-Reader";

// HTML template with modern JS for file operations
static const char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>M5Manga File Browser</title>
    <style>
        body { font-family: sans-serif; margin: 20px; background: #f0f0f0; }
        .container { max-width: 900px; margin: auto; background: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
        h2 { border-bottom: 2px solid #333; padding-bottom: 10px; }
        .controls { margin-bottom: 20px; display: flex; gap: 10px; flex-wrap: wrap; align-items: center; }
        table { width: 100%; border-collapse: collapse; margin-top: 10px; }
        th, td { padding: 12px; text-align: left; border-bottom: 1px solid #ddd; }
        th { background: #eee; }
        tr:hover { background: #f9f9f9; }
        .btn { padding: 8px 16px; border: none; border-radius: 4px; cursor: pointer; color: white; text-decoration: none; font-size: 14px; }
        .btn-blue { background: #007bff; }
        .btn-red { background: #dc3545; }
        .btn-green { background: #28a745; }
        .btn-grey { background: #6c757d; }
        .btn:disabled { background: #ccc; cursor: not-allowed; }
        input[type="file"] { display: none; }
        .path { font-weight: bold; color: #555; margin-bottom: 10px; }
        .checkbox-col { width: 40px; }
        #status { margin-top: 10px; padding: 10px; border-radius: 4px; display: none; }
        .progress { width: 100%; background: #eee; border-radius: 4px; margin-top: 10px; display: none; }
        .progress-bar { width: 0%; height: 20px; background: #28a745; border-radius: 4px; text-align: center; color: white; line-height: 20px; font-size: 12px; }
    </style>
</head>
<body>
    <div class="container">
        <h2>M5Manga File Browser</h2>
        <div class="path" id="currentPath">/</div>
        <div class="controls">
            <button class="btn btn-blue" onclick="document.getElementById('fileInput').click()">Upload Files</button>
            <button class="btn btn-blue" onclick="document.getElementById('folderInput').click()">Upload Folder</button>
            <button class="btn btn-red" id="btnDeleteSelected" onclick="deleteSelected()" disabled>Delete Selected</button>
            <button class="btn btn-grey" onclick="goBack()">Back</button>
            <input type="file" id="fileInput" multiple onchange="uploadFiles(this.files)">
            <input type="file" id="folderInput" webkitdirectory mozdirectory msdirectory odirectory directory onchange="uploadFiles(this.files)">
        </div>
        
        <div id="status"></div>
        <div class="progress"><div id="progressBar" class="progress-bar">0%</div></div>

        <table id="fileTable">
            <thead>
                <tr>
                    <th class="checkbox-col"><input type="checkbox" id="selectAll" onclick="toggleSelectAll(this)"></th>
                    <th>Name</th>
                    <th>Size</th>
                    <th>Actions</th>
                </tr>
            </thead>
            <tbody id="fileList"></tbody>
        </table>
    </div>

    <script>
        let currentDir = "/";
        
        async function loadFiles() {
            try {
                const response = await fetch(`/list?dir=${encodeURIComponent(currentDir)}`);
                const files = await response.json();
                const list = document.getElementById('fileList');
                list.innerHTML = "";
                document.getElementById('currentPath').innerText = currentDir;
                document.getElementById('selectAll').checked = false;
                updateDeleteButton();

                files.forEach(file => {
                    const tr = document.createElement('tr');
                    const isDir = file.type === "dir";
                    tr.innerHTML = `
                        <td class="checkbox-col"><input type="checkbox" class="file-check" data-path="${file.name}" onchange="updateDeleteButton()"></td>
                        <td>${isDir ? '📁' : '📄'} <a href="#" onclick="${isDir ? `changeDir('${file.name}')` : `downloadFile('${file.name}')`}">${file.name}</a></td>
                        <td>${isDir ? '-' : formatBytes(file.size)}</td>
                        <td>
                            <button class="btn btn-red" onclick="deleteFile('${file.name}')">Delete</button>
                            <button class="btn btn-blue" onclick="renameFile('${file.name}')">Rename</button>
                        </td>
                    `;
                    list.appendChild(tr);
                });
            } catch (e) {
                showStatus("Error loading files", "red");
            }
        }

        function changeDir(name) {
            if (currentDir === "/") currentDir = "/" + name;
            else currentDir = currentDir + "/" + name;
            loadFiles();
        }

        function goBack() {
            if (currentDir === "/") return;
            const parts = currentDir.split("/");
            parts.pop();
            currentDir = parts.join("/") || "/";
            loadFiles();
        }

        function formatBytes(bytes) {
            if (bytes === 0) return '0 Bytes';
            const k = 1024;
            const sizes = ['Bytes', 'KB', 'MB', 'GB'];
            const i = Math.floor(Math.log(bytes) / Math.log(k));
            return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
        }

        function toggleSelectAll(master) {
            document.querySelectorAll('.file-check').forEach(cb => cb.checked = master.checked);
            updateDeleteButton();
        }

        function updateDeleteButton() {
            const checked = document.querySelectorAll('.file-check:checked').length;
            document.getElementById('btnDeleteSelected').disabled = checked === 0;
        }

        async function deleteFile(name) {
            if (!confirm(`Delete ${name}?`)) return;
            const fullPath = (currentDir === "/" ? "" : currentDir) + "/" + name;
            await performDelete([fullPath]);
        }

        async function deleteSelected() {
            const checks = document.querySelectorAll('.file-check:checked');
            if (checks.length === 0) return;
            if (!confirm(`Delete ${checks.length} selected items?`)) return;
            const paths = Array.from(checks).map(cb => (currentDir === "/" ? "" : currentDir) + "/" + cb.getAttribute('data-path'));
            await performDelete(paths);
        }

        async function performDelete(paths) {
            showStatus("Deleting...", "blue");
            try {
                const response = await fetch('/delete', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ paths })
                });
                if (response.ok) {
                    showStatus("Deleted successfully", "green");
                    loadFiles();
                } else {
                    showStatus("Delete failed", "red");
                }
            } catch (e) { showStatus("Error during delete", "red"); }
        }

        function renameFile(name) {
            const newName = prompt("New name:", name);
            if (!newName || newName === name) return;
            const oldPath = (currentDir === "/" ? "" : currentDir) + "/" + name;
            const newPath = (currentDir === "/" ? "" : currentDir) + "/" + newName;
            
            fetch(`/rename?old=${encodeURIComponent(oldPath)}&new=${encodeURIComponent(newPath)}`)
                .then(r => {
                    if (r.ok) loadFiles();
                    else alert("Rename failed");
                });
        }

        async function uploadFiles(files) {
            if (files.length === 0) return;
            
            const progressBar = document.getElementById('progressBar');
            const progressContainer = document.querySelector('.progress');
            progressContainer.style.display = 'block';
            showStatus(`Uploading ${files.length} files...`, "blue");

            for (let i = 0; i < files.length; i++) {
                const file = files[i];
                const relativePath = file.webkitRelativePath || file.name;
                const fullPath = (currentDir === "/" ? "" : currentDir) + "/" + relativePath;
                
                progressBar.innerText = `Uploading ${i+1}/${files.length}: ${file.name}`;
                const formData = new FormData();
                formData.append('file', file, fullPath);

                try {
                    const response = await fetch('/upload', {
                        method: 'POST',
                        body: formData
                    });
                    if (!response.ok) throw new Error("Upload failed");
                    
                    const percent = Math.round(((i + 1) / files.length) * 100);
                    progressBar.style.width = percent + "%";
                } catch (e) {
                    showStatus(`Error uploading ${file.name}`, "red");
                    break;
                }
            }
            
            showStatus("Upload complete", "green");
            setTimeout(() => { progressContainer.style.display = 'none'; }, 2000);
            loadFiles();
        }

        function showStatus(msg, color) {
            const s = document.getElementById('status');
            s.innerText = msg;
            s.style.display = 'block';
            s.style.background = color === "red" ? "#f8d7da" : (color === "green" ? "#d4edda" : "#d1ecf1");
            s.style.color = color === "red" ? "#721c24" : (color === "green" ? "#155724" : "#0c5460");
        }

        loadFiles();
    </script>
</body>
</html>
)rawliteral";

// ---- path helpers: web "/" maps to SD mount "/sdcard" --------------------
static std::string web_to_fs(const std::string &web_path) {
  if (web_path.empty() || web_path == "/") return "/sdcard";
  if (!web_path.empty() && web_path[0] == '/')
    return std::string("/sdcard") + web_path;
  return std::string("/sdcard/") + web_path;
}

static std::string url_decode(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      char hex[3] = {s[i + 1], s[i + 2], 0};
      out += (char)strtol(hex, nullptr, 16);
      i += 2;
    } else if (s[i] == '+') {
      out += ' ';
    } else {
      out += s[i];
    }
  }
  return out;
}

static bool get_query_param(httpd_req_t *req, const char *key,
                            std::string &out) {
  size_t len = httpd_req_get_url_query_len(req);
  if (len == 0) return false;
  std::string buf;
  buf.resize(len + 1);
  if (httpd_req_get_url_query_str(req, buf.data(), len + 1) != ESP_OK)
    return false;
  char val[512];
  if (httpd_query_key_value(buf.c_str(), key, val, sizeof(val)) != ESP_OK)
    return false;
  out = url_decode(val);
  return true;
}

static void mkdir_p(const std::string &path) {
  if (path.empty() || path == "/") return;
  std::string cur;
  std::string work = path;
  if (!work.empty() && work[0] == '/') cur = "";
  size_t i = 0;
  // Skip leading "/sdcard" creation (mount point exists).
  // Generic mkdir -p that tolerates EEXIST.
  std::string acc;
  size_t pos = 0;
  if (work[0] == '/') {
    acc = "";
    pos = 1;
  }
  while (pos <= work.size()) {
    size_t slash = work.find('/', pos);
    std::string part;
    if (slash == std::string::npos) {
      part = work.substr(pos);
      pos = work.size() + 1;
    } else {
      part = work.substr(pos, slash - pos);
      pos = slash + 1;
    }
    if (part.empty()) continue;
    acc += "/" + part;
    if (acc == "/sdcard") continue;
    mkdir(acc.c_str(), 0755);
  }
  (void)cur;
  (void)i;
}

static bool rm_rf(const std::string &path) {
  struct stat st = {};
  if (stat(path.c_str(), &st) != 0) return false;
  if (S_ISDIR(st.st_mode)) {
    DIR *d = opendir(path.c_str());
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
      std::string n = e->d_name;
      if (n == "." || n == "..") continue;
      if (!rm_rf(path + "/" + n)) {
        closedir(d);
        return false;
      }
    }
    closedir(d);
    return rmdir(path.c_str()) == 0;
  }
  return unlink(path.c_str()) == 0;
}

static std::string json_escape(const std::string &s) {
  std::string o;
  o.reserve(s.size() + 2);
  for (char c : s) {
    if (c == '"' || c == '\\')
      o += '\\', o += c;
    else if (c == '\n')
      o += "\\n";
    else if (c == '\r')
      o += "\\r";
    else
      o += c;
  }
  return o;
}

// ---- handlers -------------------------------------------------------------
static esp_err_t handle_root(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_list(httpd_req_t *req) {
  std::string dir = "/";
  get_query_param(req, "dir", dir);
  if (dir.empty()) dir = "/";
  std::string fs = web_to_fs(dir);

  DIR *d = opendir(fs.c_str());
  if (!d) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
    return ESP_OK;
  }
  std::string out = "[";
  bool first = true;
  struct dirent *e;
  while ((e = readdir(d)) != nullptr) {
    std::string n = e->d_name;
    if (n == "." || n == "..") continue;
    if (!n.empty() && n[0] == '.') continue;
    std::string full = fs + "/" + n;
    struct stat st = {};
    bool is_dir = false;
    size_t sz = 0;
    if (stat(full.c_str(), &st) == 0) {
      is_dir = S_ISDIR(st.st_mode);
      sz = is_dir ? 0 : (size_t)st.st_size;
    }
    if (!first) out += ",";
    first = false;
    out += "{\"name\":\"" + json_escape(n) + "\",\"type\":\"";
    out += (is_dir ? "dir" : "file");
    out += "\",\"size\":";
    out += std::to_string(sz);
    out += "}";
  }
  closedir(d);
  out += "]";
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, out.c_str(), out.size());
}

static esp_err_t handle_delete(httpd_req_t *req) {
  size_t total = req->content_len;
  if (total == 0 || total > 64 * 1024) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad length");
    return ESP_OK;
  }
  // Body buffer in PSRAM: a 64KB DRAM string could fail to allocate
  // under memory pressure (fatal without exceptions), and this handler
  // runs while reader sprites occupy internal RAM.
  char *raw = (char *)heap_caps_malloc(total + 1, MALLOC_CAP_SPIRAM);
  if (!raw) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    return ESP_OK;
  }
  size_t received = 0;
  while (received < total) {
    int r = httpd_req_recv(req, raw + received, total - received);
    if (r <= 0) break;
    received += (size_t)r;
  }
  raw[received] = '\0';
  std::string_view body(raw, received);
  // Minimal JSON parse for {"paths":["/a", "/b"]}
  size_t lb = body.find('[');
  size_t rb = body.rfind(']');
  if (lb == std::string_view::npos || rb == std::string_view::npos ||
      rb <= lb) {
    heap_caps_free(raw);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_OK;
  }
  std::string_view slice = body.substr(lb + 1, rb - lb - 1);
  std::string inner(slice.data(), slice.size());
  size_t pos = 0;
  while (pos < inner.size()) {
    size_t q1 = inner.find('"', pos);
    if (q1 == std::string::npos) break;
    size_t q2 = inner.find('"', q1 + 1);
    if (q2 == std::string::npos) break;
    std::string web = inner.substr(q1 + 1, q2 - q1 - 1);
    ESP_LOGI(TAG, "Deleting: %s", web.c_str());
    std::string fs = web_to_fs(web);
    rm_rf(fs);
    // A removed top-level /manga/<entry> orphans its cached cover.
    if (web.size() > 7 && web.compare(0, 7, "/manga/") == 0) {
      std::string rest = web.substr(7);
      if (!rest.empty() && rest.find('/') == std::string::npos)
        thumb_purge_for(rest);
    }
    pos = q2 + 1;
  }
  heap_caps_free(raw);
  return httpd_resp_send(req, "OK", 2);
}

static esp_err_t handle_rename(httpd_req_t *req) {
  std::string oldp, newp;
  if (!get_query_param(req, "old", oldp) ||
      !get_query_param(req, "new", newp) || oldp.empty() ||
      newp.empty()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Rename failed");
    return ESP_OK;
  }
  std::string fs_old = web_to_fs(oldp);
  std::string fs_new = web_to_fs(newp);
  // Ensure destination parent exists.
  size_t slash = fs_new.rfind('/');
  if (slash != std::string::npos) mkdir_p(fs_new.substr(0, slash));
  if (rename(fs_old.c_str(), fs_new.c_str()) == 0)
    return httpd_resp_send(req, "OK", 2);
  httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Rename failed");
  return ESP_OK;
}

// Streaming multipart/form-data upload: one file per POST (matches web UI).
static esp_err_t handle_upload(httpd_req_t *req) {
  char ctype[256] = {};
  if (httpd_req_get_hdr_value_str(req, "Content-Type", ctype,
                                  sizeof(ctype)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No content type");
    return ESP_OK;
  }
  std::string ct = ctype;
  size_t bpos = ct.find("boundary=");
  if (bpos == std::string::npos) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No boundary");
    return ESP_OK;
  }
  std::string boundary = "--" + ct.substr(bpos + 9);
  // Strip quotes if present.
  if (!boundary.empty() && boundary.back() == '"') boundary.pop_back();
  // boundary may be quoted at start ("--\"..."); handle simply.
  {
    size_t q = boundary.find('"');
    if (q != std::string::npos) boundary.erase(q, 1);
  }

  const size_t CHUNK = 8192;
  std::vector<char> buf(CHUNK + 1);
  std::string carry;  // unprocessed bytes from previous chunk
  carry.reserve(CHUNK * 2);
  FILE *out = nullptr;
  std::string out_fs;
  bool headers_done = false;
  bool done = false;
  size_t remaining = req->content_len;  // 0 if chunked (UI sends length)

  auto close_out = [&]() {
    if (out) {
      fclose(out);
      out = nullptr;
    }
  };

  // Read until client closes or we see the closing boundary.
  while (!done) {
    int r = httpd_req_recv(req, buf.data(), CHUNK);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (r <= 0) break;
    buf[(size_t)r] = '\0';
    carry.append(buf.data(), (size_t)r);
    if (!headers_done) {
      size_t hdr_end = carry.find("\r\n\r\n");
      if (hdr_end == std::string::npos) {
        if (carry.size() > 64 * 1024) break;  // headers too big
        continue;
      }
      std::string hdrs = carry.substr(0, hdr_end);
      // Extract filename="...". FormData sends full UI path as filename.
      std::string filename;
      size_t fn = hdrs.find("filename=\"");
      if (fn != std::string::npos) {
        size_t s = fn + 10;
        size_t e = hdrs.find('"', s);
        if (e != std::string::npos) filename = hdrs.substr(s, e - s);
      }
      if (filename.empty()) break;
      // Normalize: browsers may send full path; strip directories handled
      // by UI (it already passes fullPath as filename).
      std::string web = filename;
      if (web.empty() || web[0] != '/') web = "/" + web;
      out_fs = web_to_fs(web);
      size_t slash = out_fs.rfind('/');
      if (slash != std::string::npos) mkdir_p(out_fs.substr(0, slash));
      ESP_LOGI(TAG, "Upload Start: %s", out_fs.c_str());
      out = fopen(out_fs.c_str(), "wb");
      if (!out) {
        ESP_LOGE(TAG, "Failed to open %s for writing", out_fs.c_str());
        break;
      }
      carry.erase(0, hdr_end + 4);
      headers_done = true;
    }
    if (headers_done) {
      // Look for delimiter "\r\n<boundary>" marking end of file data.
      std::string delim = "\r\n" + boundary;
      size_t d = carry.find(delim);
      if (d != std::string::npos) {
        if (out && d > 0) fwrite(carry.data(), 1, d, out);
        close_out();
        done = true;
        break;
      }
      // Keep tail that could contain a split delimiter.
      size_t keep = delim.size();
      if (carry.size() > keep) {
        size_t w = carry.size() - keep;
        if (out) fwrite(carry.data(), 1, w, out);
        carry.erase(0, w);
      }
    }
    (void)remaining;
  }
  close_out();
  if (!headers_done) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
    return ESP_OK;
  }
  return httpd_resp_send(req, "OK", 2);
}

void startWifiServer() {
  if (running) return;

  esp_netif_init();
  esp_err_t err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
    return;
  }
  esp_netif_create_default_wifi_ap();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);

  wifi_config_t ap = {};
  snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", apSSID.c_str());
  ap.ap.ssid_len = strlen(apSSID.c_str());
  ap.ap.channel = 1;
  ap.ap.max_connection = 4;
  ap.ap.authmode = WIFI_AUTH_OPEN;
  esp_wifi_set_mode(WIFI_MODE_AP);
  esp_wifi_set_config(WIFI_IF_AP, &ap);
  esp_wifi_start();

  httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
  hcfg.max_uri_handlers = 12;
  hcfg.recv_wait_timeout = 10;
  hcfg.send_wait_timeout = 10;
  if (httpd_start(&s_server, &hcfg) != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed");
    return;
  }
  httpd_uri_t u_root = {.uri = "/",
                        .method = HTTP_GET,
                        .handler = handle_root,
                        .user_ctx = nullptr};
  httpd_register_uri_handler(s_server, &u_root);
  httpd_uri_t u_list = {.uri = "/list",
                        .method = HTTP_GET,
                        .handler = handle_list,
                        .user_ctx = nullptr};
  httpd_register_uri_handler(s_server, &u_list);
  httpd_uri_t u_del = {.uri = "/delete",
                       .method = HTTP_POST,
                       .handler = handle_delete,
                       .user_ctx = nullptr};
  httpd_register_uri_handler(s_server, &u_del);
  httpd_uri_t u_ren = {.uri = "/rename",
                       .method = HTTP_GET,
                       .handler = handle_rename,
                       .user_ctx = nullptr};
  httpd_register_uri_handler(s_server, &u_ren);
  httpd_uri_t u_up = {.uri = "/upload",
                      .method = HTTP_POST,
                      .handler = handle_upload,
                      .user_ctx = nullptr};
  httpd_register_uri_handler(s_server, &u_up);

  running = true;
  ESP_LOGI(TAG, "WiFi Server Started, SSID=%s IP=%s", apSSID.c_str(),
           getWifiIP().c_str());
}

void stopWifiServer() {
  if (!running) return;
  if (s_server) {
    httpd_stop(s_server);
    s_server = nullptr;
  }
  esp_wifi_stop();
  running = false;
  ESP_LOGI(TAG, "WiFi Server Stopped");
}

void updateWifiServer() {
  // esp_http_server runs in its own task; nothing to poll.
}

bool isWifiServerRunning() { return running; }
std::string getWifiIP() { return "192.168.4.1"; }
std::string getWifiSSID() { return apSSID; }
