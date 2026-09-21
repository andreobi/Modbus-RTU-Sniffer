
const char index_html[] PROGMEM =  R"=====(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>Modbus Sniffer</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 20px; background: #f4f4f9; color: #333; }
        table { border-collapse: collapse; width: 100%; margin-top: 15px; box-shadow: 0 2px 5px rgba(0,0,0,0.05); background: white; }
        th, td { border: 1px solid #ddd; padding: 10px; text-align: center; }
        th { background-color: #007bff; color: white; }
        tr:nth-child(even) { background-color: #f8f9fa; }
        .controls { margin-bottom: 20px; background: white; padding: 15px; border-radius: 8px; box-shadow: 0 2px 5px rgba(0,0,0,0.05); }
        button { padding: 10px 15px; margin-right: 5px; border: none; border-radius: 4px; cursor: pointer; font-weight: bold; color: white; }
        .btn-start { background: #28a745; } .btn-start:hover { background: #218838; }
        .btn-stop { background: #dc3545; } .btn-stop:hover { background: #bd2130; }
        .btn-reset { background: #ffc107; color: #333; } .btn-reset:hover { background: #e0a800; }
        .btn-json { background: #17a2b8; float: right; } .btn-json:hover { background: #138496; }
        input, select { padding: 8px; margin-right: 10px; border: 1px solid #ccc; border-radius: 4px; }
        input { width: 60px; }
        .log-container { margin-top: 30px; }
        .badge { padding: 4px 8px; border-radius: 4px; color: white; font-size: 12px; font-weight: bold; display: inline-block; width: 75px; text-align: center; }
        .req { background: #17a2b8; } .res { background: #28a745; } .err { background: #dc3545; }
        .status-indicator { font-weight: bold; margin-left: 15px; padding: 5px 10px; border-radius: 4px; }
        .status-active { background: #28a745; color: white; }
        .status-armed { background: #d4edda; color: black; }
        .status-idle { background: #fff3cd; color: #856404; }
        .config-group { display: inline-block; border-left: 2px solid #ccc; padding-left: 15px; margin-left: 10px; }
        
        body { font-family: sans-serif; padding: 20px; }
        .hex-input { width: 480px; padding: 8px; font-family: monospace; font-size: 16px; }
        .error-msg { color: red; font-size: 14px; margin-top: 5px; height: 20px; }
        .json-output { margin-top: 15px; padding: 10px; background: #f4f4f4; border: 1px solid #ccc; 
                    font-family: monospace; width: 850px; word-break: break-all; }
    </style>
</head>
<body>
    <h2>📡 Modbus RTU Live Sniffer</h2>
        <table><tr><td>
        <td><div class="config-group">
            <label>Baudrate: </label>
            <select id="baudSelect" onchange="updateSerialConfig()">
                <option value="1200">1200</option> <option value="2400">2400</option>
                <option value="4800">4800</option> <option value="9600" selected>9600</option>
                <option value="19200">19200</option> <option value="38400">38400</option>
                <option value="57600">57600</option> <option value="115200">115200</option>
            </select>

            <label>Protokoll: </label>
            <select id="configSelect" onchange="updateSerialConfig()">
                <option value="SERIAL_8N1" selected>8N1</option>
                <option value="SERIAL_8E1">8E1</option>
                <option value="SERIAL_8O1">8O1</option>
            </select>
    </div></td>
    <td><button onclick="downloadJsonFile()" class="btn-json">📊 JSON Herunterladen</button></td>
    <td><a href="/setWiFi">WiFi Conifuration</a></td><</tr></table>
    <br>

    <div class="controls">
        <label>ID Filter (0:Alle) </label>
        <input type="number" id="filterInput" min="0" max="247" value="0">
        <button onclick="setFilter()" style="background:#6c757d;">Filter setzen</button>
        <button onclick="sendCommand('start')" class="btn-start">▶ Start</button>
        <button onclick="sendCommand('stop')" class="btn-stop">⏸ Stopp</button>
        <button onclick="sendCommand('reset')" class="btn-reset">🔄 Reset</button>
        <span id="statusLabel" class="status-indicator status-idle">Inaktiv</span>
    </div>

    <h3>Trigger pattern</h3>
    <label style="display: block; margin-bottom: 5px;">enter 1- 16 Hex Bytes: </label>
    <input type="text" id="hexTrigger" class="hex-input" placeholder="01 04 1F 2 4B" autocomplete="off" >
    <input type="text" id="hexMask" class="hex-input" placeholder="FF ff 7F 00 00" autocomplete="off" >
    <div id="errorMessage" class="error-msg"></div>
    
    <div id="jsonTrigger" class="json-output">[]</div>
    <div id="jsonMask" class="json-output">[]</div>
    
    <button id="setTrigger" onclick="updateTrigger()" style="background:#6c757d;">set</button> </td>


    <h3>Statistik-Matrix</h3>
    <table> <thead> <tr>
        <th>Registertype</th> <th>captured Register-Addresses</th>
        <th>Reads</th> <th>Writes</th> <th>Exception</th>
    </tr> </thead> <tbody id="matrixBody"></tbody>
    </table>
    
    <h3>Status Information</h3>
    <table> <thead> <tr>
        <th>Device List</th> <th>Fragments</th> <th>CRC Erros</th>
        <th>NO Response</th> <th>NO Request</th>
    </tr> </thead> <tbody id="statusBody"></tbody>
    </table>

    <h3>Function Code Data Monitor</h3>
    <table style="width: 100%; table-layout: fixed;"><thead>
        <tr><th style="width: 10%;" >Time</th> <th style="width: 15%;" >Device</th> <th style="width: 10%;" >FC</th>
            <th style="width: 15%;" >Address</th> <th>Data</th></tr></thead>
        <tbody">
        <tr><td id=fcvtime0></td>
        <td><input type="number" id="fcvdev0" min="0" max="247" value="0">
            <button id="cvdev0" onclick="updateFCbtn(this)" style="background:#6c757d;">set</button> </td>
        <td> <select id="fcvsel0" onchange="updateFCsel(this)"> <option value="0">none</option> 
                <option value="1">Coil  01</option> <option value="2">Disce 02</option> <option value="3" selected">Holdr 03</option>
                <option value="4">Input 04</option> <option value="5">Coilw 05</option> <option value="6">Holdw 06</option>
                <option value="15">Coilw 0f</option> <option value="16">Holdw 10</option>
            </select> </td>
        <td><input type="number" id="fcvadr0" min="0" max="65535" value="0">
            <button id="cvadr0" onclick="updateFCbtn(this)" style="background:#6c757d;">set</button> </td>
        <td id=fcvdata0></td></tr>
        
        <tr><td id=fcvtime1></td>
        <td><input type="number" id="fcvdev1" min="0" max="247" value="0">
            <button id="cvdev1" onclick="updateFCbtn(this)" style="background:#6c757d;">set</button> </td>
        <td> <select id="fcvsel1" onchange="updateFCsel(this)"> <option value="0">none</option> 
                <option value="1">Coil  01</option> <option value="2">Disce 02</option> <option value="3" selected">Holdr 03</option>
                <option value="4">Input 04</option> <option value="5">Coilw 05</option> <option value="6">Holdw 06</option>
                <option value="15">Coilw 0f</option> <option value="16">Holdw 10</option>
            </select> </td>
        <td><input type="number" id="fcvadr1" min="0" max="65535" value="0">
            <button id="cvadr1" onclick="updateFCbtn(this)" style="background:#6c757d;">set</button> </td>
        <td id=fcvdata1></td></tr>

        <tr><td id=fcvtime2></td>
        <td><input type="number" id="fcvdev2" min="0" max="247" value="0">
            <button id="cvdev2" onclick="updateFCbtn(this)" style="background:#6c757d;">set</button> </td>
        <td> <select id="fcvsel2" onchange="updateFCsel(this)"> <option value="0">none</option> 
                <option value="1">Coil  01</option> <option value="2">Disce 02</option> <option value="3" selected">Holdr 03</option>
                <option value="4">Input 04</option> <option value="5">Coilw 05</option> <option value="6">Holdw 06</option>
                <option value="15">Coilw 0f</option> <option value="16">Holdw 10</option>
            </select> </td>
        <td><input type="number" id="fcvadr2" min="0" max="65535" value="0">
            <button id="cvadr2" onclick="updateFCbtn(this)" style="background:#6c757d;">set</button> </td>
        <td id=fcvdata2></td></tr>

        <tr><td id=fcvtime3></td>
        <td><input type="number" id="fcvdev3" min="0" max="247" value="0">
            <button id="cvdev3" onclick="updateFCbtn(this)" style="background:#6c757d;">set</button> </td>
        <td> <select id="fcvsel3" onchange="updateFCsel(this)"> <option value="0">none</option> 
                <option value="1">Coil  01</option> <option value="2">Disce 02</option> <option value="3" selected">Holdr 03</option>
                <option value="4">Input 04</option> <option value="5">Coilw 05</option> <option value="6">Holdw 06</option>
                <option value="15">Coilw 0f</option> <option value="16">Holdw 10</option>
            </select> </td>
        <td><input type="number" id="fcvadr3" min="0" max="65535" value="0">
            <button id="cvadr3" onclick="updateFCbtn(this)" style="background:#6c757d;">set</button> </td>
        <td id=fcvdata3></td></tr>
        </tbody>
    </table>

    <div class="log-container">
        <h3>Chronologische Liste <span id="logCounter">(0/100)</span></h3>
        <table> <thead> <tr>
            <th>Time (ms)</th> <th>Distance</th> <th>Device</th> <th>FC</th> <th>Address</th> <th>P-Type</th>
            <th>R-Type</th> <th>R / W</th> <th>Length</th> <th>Payload (hex)</th> <th>Status</th>
        </tr> </thead> <tbody id="logBody"></tbody> </table>
    </div>


    <script>
        let ws = new WebSocket('ws://' + window.location.hostname + ':81/');
        let currentRawJson = ""; 
        let updateTimeoutId = null;
        let allowupdate = true;
        
        const hexTrigger = document.getElementById('hexTrigger');
        const jsonTrigger = document.getElementById('jsonTrigger');
        const hexMask = document.getElementById('hexMask');
        const jsonMask = document.getElementById('jsonMask');
        const errorMsg = document.getElementById('errorMessage');
        let triggerPayload = [];
        let maskPayload = [];

        function setEnableUpdate(status) {
            allowupdate = status;
        }

// set server data update lock for input/select interaction
        function starteUpdateSperre() {
            setEnableUpdate(false);
            if (updateTimeoutId !== null) {         // previous timout still active?
                clearTimeout(updateTimeoutId);
            }
    
            updateTimeoutId = setTimeout(() => {
                setEnableUpdate(true);
                updateTimeoutId = null; // Timer-Referenz zurücksetzen
            }, 5000);
        }

// set Event-Listener for all Inputs und Selects
        document.querySelectorAll('input, select').forEach(element => {
            const eventType = element.tagName === 'SELECT' ? 'change' : 'input';   // 'change' für Auswahlfelder, 'input' für Text-/Zahlenfelder
            element.addEventListener(eventType, starteUpdateSperre);
        });

// trigger value pattern input
        function updateJsonArray(value) {
            let cleanValue = value.trim();
            let bytes = cleanValue === "" ? [] : cleanValue.split(' ');
            bytes = bytes.map(b => {
                let formatted = b.toUpperCase();
                if (formatted.length === 1) {   //  "A" => "0A"
                    formatted = "0" + formatted;
                }
                return "0x" + formatted;
            });

            while (bytes.length < 16) { 	// Array = 16 x "0x00"
                bytes.push("0x00");
            }
        
            triggerPayload = bytes;
            jsonTrigger.textContent = JSON.stringify(bytes);       // In JSON-String umwandeln und anzeigen
        }

        hexTrigger.addEventListener('input', function(e) {
            let cursorPosition = this.selectionStart;
            let originalLength = this.value.length;
            
            let value = this.value.replace(/[^0-9a-fA-F ]/g, '');   // nur Hex-Zeichen und Leerzeichen
            value = value.replace(/  +/g, ' ');                     // Verhindere doppelte Leerzeichen
            let parts = value.split(' ');                           // Byte auf max. 2 Zeichen (00-FF) begrenzen
            let hasTruncatedByte = false;
            
            for (let i = 0; i < parts.length; i++) {
                if (parts[i].length > 2) {
                    parts[i] = parts[i].substring(0, 2);
                    hasTruncatedByte = true;
                }
            }
            value = parts.join(' ');
            
            if (parts.length > 16) {                                // Auf maximal 16 Bytes limitieren
                errorMsg.textContent = "Maximal 16 Bytes!";
                value = parts.slice(0, 16).join(' ');
            } else if (hasTruncatedByte) {
                errorMsg.textContent = "Byte format 2 Hex-Zeichen 00-FF!";
            } else {
                errorMsg.textContent = "";
            }
            this.value = value;
            
            let lengthDifference = originalLength - this.value.length;  // Cursor-Position korrigieren
            this.setSelectionRange(cursorPosition - lengthDifference, cursorPosition - lengthDifference);

            updateJsonArray(this.value);                    // JSON direkt live aktualisieren
        });

        // Automatische Formatierung bei Fokusverlust
        hexTrigger.addEventListener('blur', function() {
            this.value = this.value.toUpperCase();
            updateJsonArray(this.value);
        });

        // Initiale Ausführung für das leere Feld (zeigt 16x "0x00" an)
        updateJsonArray("");
        

        function updateTrigger() {
            ws.send(JSON.stringify({setTrigger: triggerPayload, setMask: maskPayload}));
        }

// trigger mask pattern input
        function updateJsonArrayM(value) {
            let cleanValue = value.trim();
            let bytes = cleanValue === "" ? [] : cleanValue.split(' ');
            bytes = bytes.map(b => {
                let formatted = b.toUpperCase();
                if (formatted.length === 1) {   //  "A" => "0A"
                    formatted = "0" + formatted;
                }
                return "0x" + formatted;
            });

            while (bytes.length < 16) { 	// Array = 16 x "0x00"
                bytes.push("0x00");
            }
        
            maskPayload = bytes;
            jsonMask.textContent = JSON.stringify(bytes);       // In JSON-String umwandeln und anzeigen
        }

        hexMask.addEventListener('input', function(e) {
            let cursorPosition = this.selectionStart;
            let originalLength = this.value.length;
            
            let value = this.value.replace(/[^0-9a-fA-F ]/g, '');   // nur Hex-Zeichen und Leerzeichen
            value = value.replace(/  +/g, ' ');                     // Verhindere doppelte Leerzeichen
            let parts = value.split(' ');                           // Byte auf max. 2 Zeichen (00-FF) begrenzen
            let hasTruncatedByte = false;
            
            for (let i = 0; i < parts.length; i++) {
                if (parts[i].length > 2) {
                    parts[i] = parts[i].substring(0, 2);
                    hasTruncatedByte = true;
                }
            }
            value = parts.join(' ');
            
            if (parts.length > 16) {                                // Auf maximal 16 Bytes limitieren
                errorMsg.textContent = "Maximal 16 Bytes!";
                value = parts.slice(0, 16).join(' ');
            } else if (hasTruncatedByte) {
                errorMsg.textContent = "Byte format 2 Hex-Zeichen 00-FF!";
            } else {
                errorMsg.textContent = "";
            }
            this.value = value;
            
            let lengthDifference = originalLength - this.value.length;  // Cursor-Position korrigieren
            this.setSelectionRange(cursorPosition - lengthDifference, cursorPosition - lengthDifference);

            updateJsonArrayM(this.value);                    // JSON direkt live aktualisieren
        });

        // Automatische Formatierung bei Fokusverlust
        hexMask.addEventListener('blur', function() {
            this.value = this.value.toUpperCase();
            updateJsonArrayM(this.value);
        });

        // Initiale Ausführung für das leere Feld (zeigt 16x "0x00" an)
        updateJsonArrayM("");
        

        function updateMask() {
            ws.send(JSON.stringify({setMask: maskPayload, setMask: maskPayload}));
        }


// receive server data
        ws.onmessage = function(event) {
            currentRawJson = event.data;
            let data = JSON.parse(event.data);
            
            if(allowupdate) {
                if (document.activeElement.id !== filterInput) {
                    document.getElementById('filterInput').value = data.filter;
                }
                if (document.activeElement.id !== document.getElementById('baudSelect')) {
                    document.getElementById('baudSelect').value = data.baudrate;
                }
                if (document.activeElement.id !== document.getElementById('configSelect')) {
                    document.getElementById('configSelect').value = data.serialConfig;
                }
            }
            let statusLabel = document.getElementById('statusLabel');
            if (data.isRecording) {
                statusLabel.innerText = "recording...";
                statusLabel.className = "status-indicator status-active";
            } else  if (data.isArmed) {
                statusLabel.innerText = "armed";
                statusLabel.className = "status-indicator status-armed";
            } else {
                statusLabel.innerText = "stopped";
                statusLabel.className = "status-indicator status-idle";
            }
            
            let matrixHtml = '';
            data.matrix.forEach(row => {
                let regList = row.regs.length > 0 ? row.regs.join(', ') : '-';
                matrixHtml += `<tr>
                    <td><strong>${row.type}</strong></td>
                    <td style="text-align:left; font-family:monospace; color:#0056b3;">${regList}</td>
                    <td>${row.req}</td>
                    <td>${row.res}</td>
                    <td>${row.err}</td>
                </tr>`;
            });
            document.getElementById('matrixBody').innerHTML = matrixHtml;

            let statusHtml = '';
            let devList = data.status.devs.length > 0 ? data.status.devs.join(', ') : '-';
                statusHtml += `<tr>
                    <td style="text-align:left; font-family:monospace; color:#0056b3;">${devList}</td>
                    <td>${data.status.frag}</td>
                    <td>${data.status.crc}</td>
                    <td>${data.status.resp}</td>
                    <td>${data.status.requ}</td>
                </tr>`;
            document.getElementById('statusBody').innerHTML = statusHtml;

            let i = 0;
            data.fcvm.forEach(row => {
                document.getElementById("fcvtime"+i).innerText =row.time;
            if(allowupdate) {
                if (document.activeElement.id !== "fcvdev"+i) {
                document.getElementById("fcvdev"+i).value =row.dev;
                }
                if (document.activeElement.id !== "fcvsel"+i) {
                document.getElementById("fcvsel"+i).value =row.fc;
                }
                if (document.activeElement.id !== "fcvadr"+i) {
                document.getElementById("fcvadr"+i).value =row.adr;
                }
            }
                document.getElementById("fcvdata"+i).innerText =row.data;
                i = i+1;
            });

            let logHtml = '';
            data.log.forEach(entry => {
                let normType = entry.pType.trim().toLowerCase();
                let badgeClass = 'err'; // Fallback
                if (normType === 'request') badgeClass = 'req';
                else if (normType === 'response') badgeClass = 'res';
                else if (normType === 'error') badgeClass = 'err';

                logHtml += `<tr>
                    <td>${entry.time}</td>
                    <td>${entry.dist}</td>
                    <td>${entry.devi}</td>
                    <td>${entry.func}</td>
                    <td>${entry.addr}</td>
                    <td><span class="badge ${badgeClass}">${entry.pType}</span></td>
                    <td><strong>${entry.regType}</strong></td>
                    <td>${entry.drw}</td>
                    <td>${entry.len}</td>
                    <td style="font-family:monospace; text-align:left;">${entry.data}</td>
                    <td>${entry.status}</td>
                </tr>`;
            });
            document.getElementById('logBody').innerHTML = logHtml;
            document.getElementById('logCounter').innerText = `(${data.log.length}/100)`;
        };

        function setFilter() {
            let val = parseInt(document.getElementById('filterInput').value) || 0;
            ws.send(JSON.stringify({setFilter: val}));
        }

        function updateFCbtn(btn) {
            const id = "f" + btn.id;
            const val = parseInt(document.getElementById(id).value) || 0;
            document.getElementById("fcvdata"+id.charAt(6)).innerText = "";
            ws.send(JSON.stringify({setFcvId: id, setFcvVal: val}));
        }
        
        function updateFCsel(sel) {
            const id = sel.id;
            const val = parseInt(sel.value) || 0;
            document.getElementById("fcvdata"+id.charAt(6)).innerText = "";
            ws.send(JSON.stringify({setFcvId: id, setFcvVal: val, setFcvData: ""}));
        }
        
        function updateSerialConfig() {
            let baud = parseInt(document.getElementById('baudSelect').value);
            let configStr = document.getElementById('configSelect').value;
            ws.send(JSON.stringify({setBaud: baud, setConfig: configStr}));
        }

        function sendCommand(cmdStr) {
            ws.send(JSON.stringify({cmd: cmdStr}));
        }

        function downloadJsonFile() {
            if(!currentRawJson) return alert("Keine Daten vorhanden!");
            let blob = new Blob([currentRawJson], {type: "application/json"});
            let a = document.createElement("a");
            a.href = URL.createObjectURL(blob);
            a.download = "modbus_sniffer_data.json";
            document.body.appendChild(a);
            a.click();
            document.body.removeChild(a);
        }
    </script>
</body>
</html>

)=====";


const char config_html[] PROGMEM = 
"<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Setup</title></head><body>"
"<h2>Modbus Sniffer WiFi Setup</h2><form action='/save-config' method='POST'>"
"SSID: <input name='ssid' required><br>PW: <input name='password' type='password' required><br>"
"<button type='submit'>Speichern</button></form></body></html>";
