#include "wifi_server.h"
#include <SD.h>
#include <WebServer.h>
#include <WiFi.h>
#include <vector>

static WebServer server(80);
static bool running = false;
static String apSSID = "M5Manga-Reader";

// HTML template with modern JS for file operations
const char INDEX_HTML[] PROGMEM = R"rawliteral(
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

static void handleRoot() { server.send_P(200, "text/html", INDEX_HTML); }

static void handleList()
{
  String path = server.arg("dir");
  if (path == "")
    path = "/";

  File root = SD.open(path);
  if (!root || !root.isDirectory())
  {
    server.send(404, "text/plain", "Not Found");
    return;
  }

  String output = "[";
  output.reserve(1024); // Start with a reasonable size
  File file = root.openNextFile();
  while (file)
  {
    if (output.length() > 1)
      output += ",";
    output += "{\"name\":\"";
    output += file.name();
    output += "\",\"type\":\"";
    output += (file.isDirectory() ? "dir" : "file");
    output += "\",\"size\":";
    output += String(file.size());
    output += "}";

    file = root.openNextFile();
  }
  output += "]";
  server.send(200, "application/json", output);
}

static void handleDelete()
{
  if (server.hasArg("plain") == false)
  {
    server.send(400, "text/plain", "Body not received");
    return;
  }
  String body = server.arg("plain");
  // Simple JSON parse for {"paths":["/path1", "/path2"]}
  int start = body.indexOf("[");
  int end = body.lastIndexOf("]");
  if (start == -1 || end == -1)
  {
    server.send(400, "text/plain", "Invalid JSON");
    return;
  }
  String pathsStr = body.substring(start + 1, end);

  while (pathsStr.length() > 0)
  {
    int nextQuote = pathsStr.indexOf("\"");
    if (nextQuote == -1)
      break;
    pathsStr = pathsStr.substring(nextQuote + 1);
    int endQuote = pathsStr.indexOf("\"");
    if (endQuote == -1)
      break;
    String path = pathsStr.substring(0, endQuote);

    Serial.printf("Deleting: %s\n", path.c_str());
    if (SD.exists(path))
    {
      File f = SD.open(path);
      if (f.isDirectory())
      {
        f.close();
        // Simple recursive delete not built-in, but we can try rmdir
        SD.rmdir(path.c_str());
      }
      else
      {
        f.close();
        SD.remove(path.c_str());
      }
    }

    pathsStr = pathsStr.substring(endQuote + 1);
    int comma = pathsStr.indexOf(",");
    if (comma != -1)
      pathsStr = pathsStr.substring(comma + 1);
    else
      break;
  }
  server.send(200, "text/plain", "OK");
}

static void handleRename()
{
  String oldPath = server.arg("old");
  String newPath = server.arg("new");
  if (oldPath != "" && newPath != "")
  {
    if (SD.rename(oldPath, newPath))
    {
      server.send(200, "text/plain", "OK");
      return;
    }
  }
  server.send(500, "text/plain", "Rename failed");
}

static void handleUpload()
{
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START)
  {
    String filename = upload.filename;
    if (!filename.startsWith("/"))
      filename = "/" + filename;

    // Ensure directories exist
    int slashIdx = filename.lastIndexOf('/');
    if (slashIdx > 0)
    {
      String dir = filename.substring(0, slashIdx);
      if (!SD.exists(dir))
      {
        // Create recursive directories
        String current = "";
        String remaining = dir;
        if (remaining.startsWith("/"))
          remaining = remaining.substring(1);
        while (remaining.length() > 0)
        {
          int nextSlash = remaining.indexOf('/');
          String part;
          if (nextSlash != -1)
          {
            part = remaining.substring(0, nextSlash);
            remaining = remaining.substring(nextSlash + 1);
          }
          else
          {
            part = remaining;
            remaining = "";
          }
          current += "/" + part;
          if (!SD.exists(current))
            SD.mkdir(current);
        }
      }
    }

    Serial.printf("Upload Start: %s\n", filename.c_str());
    File file = SD.open(filename, FILE_WRITE);
    if (!file)
    {
      Serial.println("Failed to open file for writing");
    }
    file.close(); // Will re-open in write mode during UPLOAD_FILE_WRITE
  }
  else if (upload.status == UPLOAD_FILE_WRITE)
  {
    String filename = upload.filename;
    if (!filename.startsWith("/"))
      filename = "/" + filename;
    File file = SD.open(filename, FILE_APPEND);
    if (file)
    {
      file.write(upload.buf, upload.currentSize);
      file.close();
    }
  }
  else if (upload.status == UPLOAD_FILE_END)
  {
    server.send(200, "text/plain", "OK");
  }
}

void startWifiServer()
{
  if (running)
    return;
  WiFi.softAP(apSSID.c_str());

  server.on("/", HTTP_GET, handleRoot);
  server.on("/list", HTTP_GET, handleList);
  server.on("/delete", HTTP_POST, handleDelete);
  server.on("/rename", HTTP_GET, handleRename);
  server.on("/upload", HTTP_POST, []()
            { server.send(200); }, handleUpload);

  server.begin();
  running = true;
  Serial.println("WiFi Server Started");
  Serial.print("IP: ");
  Serial.println(WiFi.softAPIP());
}

void stopWifiServer()
{
  if (!running)
    return;
  server.stop();
  WiFi.softAPdisconnect(true);
  running = false;
  Serial.println("WiFi Server Stopped");
}

void updateWifiServer()
{
  if (running)
    server.handleClient();
}

bool isWifiServerRunning() { return running; }
String getWifiIP() { return WiFi.softAPIP().toString(); }
String getWifiSSID() { return apSSID; }
