#include <Wifi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <esp_wifi.h>

// WiFi details
const char *ssid = "SSID";
const char *password = "PASSWORD";

WebServer server(80);

// Packet structure
struct Packet
{
    uint16_t packetNumber;
    uint8_t dataLength; // Data size (1-16 bytes)
    uint8_t data[16];
    uint8_t checksum; // XOR checksum value
};

void handleRoot();
void handleFileUpload();
void handleListFiles();
void handleDownloadPackets();
void handleGetPacket();
uint8_t calculateXORChecksum(uint8_t *data, uint8_t length);
void processFileIntoPackets(String filename);

void setup()
{
    Serial.begin(115200); // Set baud rate

    // Initialize flash memory (SPIFFS)
    if (!SPIFFS.begin(true))
    {
        Serial.println("An Error has occured with SPIFFS");
        return;
    }

    // Connect ESP32 to WiFi
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("Connecting to WiFi...");
    }

    // Set WiFi to low power mode
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    Serial.println("WiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    // Setup web server
    server.on("/", HTTP_GET, handleRoot);
    server.on("/upload", HTTP_POST, []()
              { server.send(200, "text/plain", ""); }, handleFileUpload);
    server.on("/list", HTTP_GET, handleListFiles);
    server.on("/packets", HTTP_GET, handleDownloadPackets);
    server.on("/packet", HTTP_GET, handleGetPacket);
    server.begin();

    Serial.println("HTTP server launched");
    Serial.println("Useful Endpoints:");
    Serial.println("/upload - Upload a file");
    Serial.println("/list - View list of files");
    Serial.println("/packets?file=filename - View JSON packet info");
    Serial.println("/packet?file=filename&num=X - Access specific packet");
}

void loop()
{
    server.handleClient(); // Keep checking for GET or POST requests
}

// Sets up root of the web server
void handleRoot()
{
    String html = "<!DOCTYPE html><html><head><title>ESP32 Binary File Transfer</title></head><body>";
    html += "<h2>ESP32 Binary File Transfer</h2>";
    html += "<h3>Upload File</h3>";
    html += "<form method='POST' action='/upload' enctype='multipart/form-data'>";
    html += "<input type='file' name='file'><br><br>";
    html += "<input type='submit' value='Upload File'>";
    html += "</form>";
    html += "<br><a href='/list'>View uploaded files</a>";
    html += "<br><br><h3>Useful Endpoints:</h3>";
    html += "<ul>";
    html += "<li><code>GET /packets?file=filename</code> - Get packet count and info</li>";
    html += "<li><code>GET /packet?file=filename&num=X</code> - Get packet X as binary</li>";
    html += "</ul>";
    html += "</body></html>";

    server.send(200, "text/html", html);
}

#define MAX_PACKETS 1024 // Max number of packets for a file
uint8_t uploadChecksums[MAX_PACKETS];
uint16_t packetIndex = 0;
uint8_t packetBuffer[16];
uint8_t packetPos = 0;

void handleFileUpload()
{
    HTTPUpload &upload = server.upload();
    static String uploadedFile = "";             // Initialize
    static std::vector<uint8_t> uploadChecksums; // Store uploaded packet checksums

    if (upload.status == UPLOAD_FILE_START)
    {
        uploadedFile = "/" + upload.filename;
        Serial.print("Upload Start: ");
        Serial.println(uploadedFile);

        // Open file for writing
        File file = SPIFFS.open(uploadedFile, "w");
        if (!file)
        {
            Serial.println("Error: Failed to open file for writing");
            return;
        }
        file.close();
        uploadChecksums.clear();
    }
    else if (upload.status == UPLOAD_FILE_WRITE)
    {
        // Append chunk to file
        File file = SPIFFS.open(uploadedFile, "a");
        if (file)
        {
            file.write(upload.buf, upload.currentSize);
            file.close();
        }

        // Compute checksum for each 16-byte packet in this chunk
        for (int i = 0; i < upload.currentSize; i += 16)
        {
            size_t len = min((size_t)16, upload.currentSize - i);
            uint8_t checksum = calculateXORChecksum(upload.buf + i, len);
            uploadChecksums.push_back(checksum);
        }
    }
    else if (upload.status == UPLOAD_FILE_END)
    {
        Serial.print("Upload End: ");
        Serial.print(upload.filename);
        Serial.print(",Size: ");
        Serial.println(upload.totalSize);
        // Open file to re-read and verify packet checksums
        File file = SPIFFS.open(uploadedFile, "r");
        if (!file)
        {
            Serial.println("Error: Failed to open file for verification");
            server.send(500, "text/plain", "Error verifying file");
            return;
        }

        String html = "<!DOCTYPE html><html><head><title>Upload Verification</title></head><body>";
        html += "<h2>Packet Verification Results for " + upload.filename + "</h2>";
        html += "<table border='1'><tr><th>Packet #</th><th>Upload Checksum</th><th>SPIFFS Checksum</th><th>Status</th></tr>";

        uint16_t packetNum = 0;
        while (file.available())
        {
            uint8_t buffer[16] = {0};
            size_t bytesRead = file.read(buffer, 16);
            uint8_t spiffsChecksum = calculateXORChecksum(buffer, bytesRead);
            uint8_t uploadChecksum = (packetNum < uploadChecksums.size()) ? uploadChecksums[packetNum] : 0xFF;

            html += "<tr><td>" + String(packetNum) + "</td>";
            html += "<td>0x" + String(uploadChecksum, HEX) + "</td>";
            html += "<td>0x" + String(spiffsChecksum, HEX) + "</td>";
            html += "<td>" + String((uploadChecksum == spiffsChecksum) ? "MATCH" : "MISMATCH") + "</td></tr>";

            packetNum++;
        }

        file.close();
        html += "</table>";
        html += "<br><a href='/list'>Back to file list</a></body></html>";

        server.send(200, "text/html", html);
        Serial.println("File verification complete!");
    }
}
void handleListFiles() {
  String html = "<!DOCTYPE html><html><head><title>Uploaded Files</title></head><body>";
  html += "<h2>Uploaded Files</h2>";
  html += "<a href='/'>← Back to upload</a><br><br>";
  
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  
  if (!file) {
    html += "<p>No files uploaded yet.</p>";
  } else {
    html += "<table border='1'><tr><th>File Name</th><th>Size</th><th>Actions</th></tr>";
    while (file) {
      String fileName = file.name();
      if (!fileName.endsWith(".pkt")) {  // Don't show packet files
        html += "<tr>";
        html += "<td>" + fileName + "</td>";
        html += "<td>" + String(file.size()) + " bytes</td>";
        html += "<td><a href='/packets?file=" + fileName.substring(0) + "'>View Packets</a></td>";
        html += "</tr>";
      }
      file = root.openNextFile();
    }
    html += "</table>";
  }
  
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleDownloadPackets() {
  String filename = server.arg("file");
  if (filename == "") {
    server.send(400, "application/json", "{\"error\":\"Missing file parameter\"}");
    return;
  }
  
  // Ensure filename starts with /
  String fullPath = filename;
  if (!fullPath.startsWith("/")) {
    fullPath = "/" + filename;
  }
  
  Serial.print("Looking for file: ");
  Serial.println(fullPath);
  
  // Check if original file exists
  if (!SPIFFS.exists(fullPath)) {
    server.send(404, "application/json", "{\"error\":\"File not found: " + fullPath + "\"}");
    return;
  }
  
  File file = SPIFFS.open(fullPath, "r");
  if (!file) {
    server.send(404, "application/json", "{\"error\":\"Cannot open file\"}");
    return;
  }
  
  size_t fileSize = file.size();
  file.close();
  
  uint16_t totalPackets = (fileSize + 15) / 16;  // Round up
  
  String response = "{";
  response += "\"filename\":\"" + filename + "\",";
  response += "\"fileSize\":" + String(fileSize) + ",";
  response += "\"totalPackets\":" + String(totalPackets) + ",";
  response += "\"packetSize\":16,";
  response += "\"usage\":\"GET /packet?file=" + filename + "&num=X (X: 0-" + String(totalPackets-1) + ")\"";
  response += "}";
  
  server.send(200, "application/json", response);
}

void handleGetPacket() {
  String filename = server.arg("file");
  String packetNumStr = server.arg("num");
  
  if (filename == "" || packetNumStr == "") {
    server.send(400, "application/json", "{\"error\":\"Missing parameters\"}");
    return;
  }
  
  // Ensure filename starts with /
  String fullPath = filename;
  if (!fullPath.startsWith("/")) {
    fullPath = "/" + filename;
  }
  
  uint16_t packetNum = packetNumStr.toInt();
  
  Serial.print("Getting packet ");
  Serial.print(packetNum);
  Serial.print(" from file: ");
  Serial.println(fullPath);
  
  // Open file
  File file = SPIFFS.open(fullPath, "r");
  if (!file) {
    server.send(404, "application/json", "{\"error\":\"File not found\"}");
    return;
  }
  
  size_t fileSize = file.size();
  uint16_t totalPackets = (fileSize + 15) / 16;
  
  if (packetNum >= totalPackets) {
    file.close();
    server.send(400, "application/json", "{\"error\":\"Packet number out of range\"}");
    return;
  }
  
  // Seek to packet position
  size_t seekPos = packetNum * 16;
  file.seek(seekPos);
  
  // Create packet
  Packet packet;
  packet.packetNumber = packetNum;
  
  // Read data
  size_t bytesRead = file.readBytes((char*)packet.data, 16);
  packet.dataLength = bytesRead;
  
  // Pad remaining bytes with 0
  for (int i = bytesRead; i < 16; i++) {
    packet.data[i] = 0;
  }
  
  // Calculate checksum
  packet.checksum = calculateXORChecksum(packet.data, packet.dataLength);
  
  file.close();
  
  // Send packet as binary data
  String binaryData = "";
  uint8_t* packetBytes = (uint8_t*)&packet;
  for (int i = 0; i < sizeof(Packet); i++) {
    binaryData += (char)packetBytes[i];
  }
  server.send(200, "application/octet-stream", binaryData);
  
  Serial.print("Sent packet ");
  Serial.print(packetNum);
  Serial.print(" with ");
  Serial.print(bytesRead);
  Serial.print(" bytes, checksum: 0x");
  Serial.println(packet.checksum, HEX);
}

uint8_t calculateXORChecksum(uint8_t* data, uint8_t length) {
  uint8_t checksum = 0;
  for (int i = 0; i < length; i++) {
    checksum ^= data[i];
  }
  return checksum;
}

void processFileIntoPackets(String filename) {
  File file = SPIFFS.open(filename, "r");
  if (!file) {
    Serial.println("Failed to open file for packet processing");
    return;
  }
  
  size_t fileSize = file.size();
  uint16_t totalPackets = (fileSize + 15) / 16;
  
  Serial.print("Processing file: ");
  Serial.println(filename);
  Serial.print("File size: ");
  Serial.print(fileSize);
  Serial.println(" bytes");
  Serial.print("Total packets: ");
  Serial.println(totalPackets);
  
  file.close();
  
  Serial.println("File ready for packet-based download!");
}