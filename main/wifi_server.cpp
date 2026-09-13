#include "wifi_server.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "compat.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "manga_wifi";

static httpd_handle_t s_server = nullptr;
static bool s_running = false;
static std::string s_apSSID = "M5Manga-Reader";
static esp_netif_t *s_ap_netif = nullptr;

// HTML template with modern JS for file operations (unchanged client side)
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

// --- Small helpers ---------------------------------------------------------

static std::string url_decode(const std::string &in)
{
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i)
  {
    if (in[i] == '%' && i + 2 < in.size())
    {
      char hex[3] = {in[i + 1], in[i + 2], 0};
      out += (char)strtol(hex, nullptr, 16);
      i += 2;
    }
    else if (in[i] == '+')
      out += ' ';
    else
      out += in[i];
  }
  return out;
}

// Raw query value for `key` (URL-decoded), or `dflt` if absent.
static std::string query_param(httpd_req_t *req, const char *key,
                               const std::string &dflt = "")
{
  size_t qlen = httpd_req_get_url_query_len(req);
  if (qlen == 0)
    return dflt;
  std::string query(qlen + 1, '\0');
  if (httpd_req_get_url_query_str(req, query.data(), query.size()) != ESP_OK)
    return dflt;
  query.resize(qlen);
  std::string needle = std::string(key) + "=";
  size_t pos = 0;
  while ((pos = query.find(needle, pos)) != std::string::npos)
  {
    if (pos == 0 || query[pos - 1] == '&' || query[pos - 1] == '?')
    {
      size_t start = pos + needle.size();
      size_t end = query.find('&', start);
      return url_decode(query.substr(start, end == std::string::npos
                                                ? end
                                                : end - start));
    }
    pos += needle.size();
  }
  return dflt;
}

// Map a browser path ("/manga/foo") to a VFS path ("/sdcard/manga/foo").
// Rejects path traversal.
static bool browser_to_vfs(const std::string &browser, std::string &vfs)
{
  if (browser.empty() || browser[0] != '/' || browser.find("..") != std::string::npos)
    return false;
  vfs = "/sdcard" + browser;
  // Collapse a trailing "/." the JS never sends; keep it simple.
  return true;
}

static void mkdir_p(const std::string &path)
{
  std::string cur;
  for (size_t i = 1; i <= path.size(); ++i)
  {
    if (i == path.size() || path[i] == '/')
    {
      cur = path.substr(0, i);
      if (!cur.empty())
        mkdir(cur.c_str(), 0755);
    }
  }
}

static void rm_recursive(const std::string &path)
{
  struct stat st;
  if (stat(path.c_str(), &st) != 0)
    return;
  if (!S_ISDIR(st.st_mode))
  {
    unlink(path.c_str());
    return;
  }
  DIR *dir = opendir(path.c_str());
  if (!dir)
    return;
  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr)
  {
    std::string name = entry->d_name;
    if (name == "." || name == "..")
      continue;
    rm_recursive(path + "/" + name);
  }
  closedir(dir);
  rmdir(path.c_str());
}

static std::string json_escape(const std::string &s)
{
  std::string out;
  out.reserve(s.size());
  for (char c : s)
  {
    if (c == '"' || c == '\\')
    {
      out += '\\';
      out += c;
    }
    else if ((unsigned char)c < 0x20)
    {
      char buf[7];
      snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    }
    else
      out += c;
  }
  return out;
}

// --- Handlers ---------------------------------------------------------------

static esp_err_t handle_root(httpd_req_t *req)
{
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_list(httpd_req_t *req)
{
  std::string browser = query_param(req, "dir", "/");
  std::string vfs;
  if (!browser_to_vfs(browser, vfs))
  {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad path");
    return ESP_FAIL;
  }

  DIR *dir = opendir(vfs.c_str());
  if (!dir)
  {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
    return ESP_FAIL;
  }

  std::string output = "[";
  output.reserve(1024);
  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr)
  {
    std::string name = entry->d_name;
    if (name == "." || name == "..")
      continue;
    std::string full = vfs + "/" + name;
    struct stat st;
    bool isDir = (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
    long size = (!isDir && stat(full.c_str(), &st) == 0) ? (long)st.st_size : 0;
    if (output.length() > 1)
      output += ",";
    output += "{\"name\":\"" + json_escape(name) + "\",\"type\":\"" +
              (isDir ? "dir" : "file") + "\",\"size\":" + std::to_string(size) +
              "}";
  }
  closedir(dir);

  output += "]";
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, output.c_str(), output.size());
}

static esp_err_t handle_delete(httpd_req_t *req)
{
  size_t len = (size_t)req->content_len;
  if (len == 0 || len > 8192)
  {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
    return ESP_FAIL;
  }
  std::string body(len + 1, '\0');
  size_t received = 0;
  while (received < len)
  {
    int ret = httpd_req_recv(req, body.data() + received, len - received);
    if (ret <= 0)
    {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv failed");
      return ESP_FAIL;
    }
    received += (size_t)ret;
  }
  body.resize(len);

  // Minimal JSON parse for {"paths":["/a", "/b"]} - same approach as before.
  size_t start = body.find('[');
  size_t end = body.rfind(']');
  if (start == std::string::npos || end == std::string::npos || end <= start)
  {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }
  std::string paths = body.substr(start + 1, end - start - 1);
  while (!paths.empty())
  {
    size_t q1 = paths.find('"');
    if (q1 == std::string::npos)
      break;
    size_t q2 = paths.find('"', q1 + 1);
    if (q2 == std::string::npos)
      break;
    std::string browser = paths.substr(q1 + 1, q2 - q1 - 1);
    std::string vfs;
    if (browser_to_vfs(browser, vfs))
    {
      ESP_LOGI(TAG, "Deleting: %s", vfs.c_str());
      rm_recursive(vfs);
    }
    size_t comma = paths.find(',', q2 + 1);
    if (comma == std::string::npos)
      break;
    paths = paths.substr(comma + 1);
  }
  return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_rename(httpd_req_t *req)
{
  std::string oldB = query_param(req, "old");
  std::string newB = query_param(req, "new");
  std::string oldV, newV;
  if (!oldB.empty() && !newB.empty() && browser_to_vfs(oldB, oldV) &&
      browser_to_vfs(newB, newV) && rename(oldV.c_str(), newV.c_str()) == 0)
  {
    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
  }
  httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Rename failed");
  return ESP_FAIL;
}

// Streaming multipart/form-data upload (one file per POST, as the JS sends).
// Writes straight to the SD card without buffering the whole file.
static esp_err_t handle_upload(httpd_req_t *req)
{
  // 1. Boundary from Content-Type.
  char ctype[128] = {0};
  if (httpd_req_get_hdr_value_str(req, "Content-Type", ctype,
                                  sizeof(ctype)) != ESP_OK)
  {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No Content-Type");
    return ESP_FAIL;
  }
  const char *b = strstr(ctype, "boundary=");
  if (!b)
  {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No boundary");
    return ESP_FAIL;
  }
  std::string boundary = "--";
  boundary += (b + 9);

  // 2. Read the part headers (first chunk).
  char chunk[4096];
  std::string head;
  int ret = 0;
  size_t hdr_end = std::string::npos;
  while (head.size() < 65536)
  {
    ret = httpd_req_recv(req, chunk, sizeof(chunk));
    if (ret <= 0)
    {
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv failed");
      return ESP_FAIL;
    }
    head.append(chunk, (size_t)ret);
    hdr_end = head.find("\r\n\r\n");
    if (hdr_end != std::string::npos)
      break;
  }
  if (hdr_end == std::string::npos)
  {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad multipart");
    return ESP_FAIL;
  }

  // 3. Target filename from the part headers.
  size_t fn = head.find("filename=\"");
  if (fn == std::string::npos || fn > hdr_end)
  {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No filename");
    return ESP_FAIL;
  }
  fn += 10;
  size_t fn_end = head.find('"', fn);
  std::string browser = head.substr(fn, fn_end - fn);
  while (!browser.empty() && browser.front() == '/')
    browser.erase(browser.begin());
  std::string vfs;
  if (!browser_to_vfs("/" + browser, vfs))
  {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad filename");
    return ESP_FAIL;
  }
  size_t slash = vfs.rfind('/');
  if (slash != std::string::npos)
    mkdir_p(vfs.substr(0, slash));

  ESP_LOGI(TAG, "Upload: %s (%d bytes)", vfs.c_str(), req->content_len);
  FILE *f = fopen(vfs.c_str(), "wb");
  if (!f)
  {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Open failed");
    return ESP_FAIL;
  }

  // 4. Stream the body, stopping at the closing boundary. The marker may be
  // split across chunks, so retain an overlap tail between iterations.
  std::string marker = "\r\n" + boundary;
  std::string buf = head.substr(hdr_end + 4);
  size_t keep = marker.size() + 8;
  bool done = false;
  // Body bytes already buffered (total received minus part-header length).
  size_t buffered_body = head.size() - (hdr_end + 4);
  size_t remaining = ((size_t)req->content_len > buffered_body)
                         ? (size_t)req->content_len - buffered_body
                         : 0;

  auto flush_upto = [&](size_t n)
  {
    if (n > 0)
      fwrite(buf.data(), 1, n, f);
    buf.erase(0, n);
  };

  // Account for bytes already in `buf` beyond the headers.
  while (!done)
  {
    size_t m = buf.find(marker);
    if (m != std::string::npos)
    {
      flush_upto(m); // drop "\r\n--boundary..." and everything after
      done = true;
      break;
    }
    if (buf.size() > keep)
      flush_upto(buf.size() - keep);
    if (remaining == 0)
      break; // malformed - no closing boundary; write what we have
    ret = httpd_req_recv(req, chunk, sizeof(chunk));
    if (ret < 0)
    {
      fclose(f);
      unlink(vfs.c_str());
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv failed");
      return ESP_FAIL;
    }
    if (ret == 0)
      break;
    buf.append(chunk, (size_t)ret);
    remaining -= (size_t)ret;
  }
  // If we exited because remaining hit 0 without seeing the marker, flush.
  if (!done)
    flush_upto(buf.size());
  fclose(f);

  // Drain any unread epilogue bytes so the keep-alive connection stays in
  // sync for the next request.
  do
  {
    ret = httpd_req_recv(req, chunk, sizeof(chunk));
  } while (ret > 0);

  if (!done)
    ESP_LOGW(TAG, "Upload %s finished without closing boundary", vfs.c_str());
  return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

// --- Lifecycle ---------------------------------------------------------------

void startWifiServer()
{
  if (s_running)
    return;

  esp_err_t ret = esp_netif_init();
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    ESP_LOGW(TAG, "esp_netif_init: %s", esp_err_to_name(ret));
  ret = esp_event_loop_create_default();
  if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    ESP_LOGW(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(ret));

  if (!s_ap_netif)
    s_ap_netif = esp_netif_create_default_wifi_ap();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

  wifi_config_t ap_config = {};
  strncpy((char *)ap_config.ap.ssid, s_apSSID.c_str(),
          sizeof(ap_config.ap.ssid) - 1);
  ap_config.ap.ssid_len = (uint8_t)s_apSSID.size();
  ap_config.ap.channel = 1;
  ap_config.ap.max_connection = 4;
  ap_config.ap.authmode = WIFI_AUTH_OPEN;
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
  http_cfg.stack_size = 8192;
  http_cfg.max_uri_handlers = 8;
  http_cfg.uri_match_fn = httpd_uri_match_wildcard;
  if (httpd_start(&s_server, &http_cfg) != ESP_OK)
  {
    ESP_LOGE(TAG, "httpd_start failed");
    return;
  }

  static const httpd_uri_t uri_root = {.uri = "/",
                                       .method = HTTP_GET,
                                       .handler = handle_root,
                                       .user_ctx = nullptr};
  static const httpd_uri_t uri_list = {.uri = "/list",
                                       .method = HTTP_GET,
                                       .handler = handle_list,
                                       .user_ctx = nullptr};
  static const httpd_uri_t uri_del = {.uri = "/delete",
                                      .method = HTTP_POST,
                                      .handler = handle_delete,
                                      .user_ctx = nullptr};
  static const httpd_uri_t uri_rename = {.uri = "/rename",
                                         .method = HTTP_GET,
                                         .handler = handle_rename,
                                         .user_ctx = nullptr};
  static const httpd_uri_t uri_upload = {.uri = "/upload",
                                         .method = HTTP_POST,
                                         .handler = handle_upload,
                                         .user_ctx = nullptr};
  httpd_register_uri_handler(s_server, &uri_root);
  httpd_register_uri_handler(s_server, &uri_list);
  httpd_register_uri_handler(s_server, &uri_del);
  httpd_register_uri_handler(s_server, &uri_rename);
  httpd_register_uri_handler(s_server, &uri_upload);

  s_running = true;
  ESP_LOGI(TAG, "WiFi AP '%s' started, browse http://%s", s_apSSID.c_str(),
           getWifiIP().c_str());
}

void stopWifiServer()
{
  if (!s_running)
    return;
  if (s_server)
  {
    httpd_stop(s_server);
    s_server = nullptr;
  }
  esp_wifi_stop();
  esp_wifi_deinit();
  s_running = false;
  ESP_LOGI(TAG, "WiFi server stopped");
}

void updateWifiServer()
{
  // httpd serves requests on its own task - nothing to poll.
}

bool isWifiServerRunning() { return s_running; }

std::string getWifiIP()
{
  if (s_ap_netif)
  {
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK)
    {
      char buf[16];
      snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
      return std::string(buf);
    }
  }
  return "192.168.4.1";
}

std::string getWifiSSID() { return s_apSSID; }
