C++ Code

--Setup--
1. InitCamera: Initializes the camera module with the configured settings, including pixel format, frame size, and JPEG quality. It also sets the camera brightness, AE level, and vertical flip.
2. InitI2C: Initializes the I2C communication and sets up the MLX90614 temperature sensor.
3. InitRadar: Initializes the UART communication and sets up the MR60BHA2 radar sensor.
4. InitI2S: Configures the I2S settings for the INMP441 microphone, including the 32-bit mode, sample rate, and DMA buffer.
5. ConnectWiFi: Connects the device to the configured WiFi network and prints the connected IP address.
6. SyncNTP: Synchronizes the device's time with NTP servers.
7. ConnectMQTT: Connects the device to the MQTT broker and handles connection failures.
8. ConnectAudio: Connects the device to the audio WebSocket server, generating a random WebSocket key and handling the handshake response.
9. ConnectVideo: Connects the device to the video WebSocket server, generating a random WebSocket key and handling the handshake response.

--Main Loop--
1. MQTTKeepAlive: Maintains the MQTT connection, reconnecting if the client becomes disconnected.
2. CaptureAudio: Reads audio data from the I2S interface, converts the 32-bit samples to 16-bit, and sends the audio frames to the WebSocket.
3. AudioReconnect: Checks if the audio WebSocket is connected and reconnects if necessary.
4. CaptureVideo: Captures a JPEG frame from the camera, sends it to the WebSocket, and releases the camera frame buffer.
5. VideoReconnect: Checks if the video WebSocket is connected and reconnects if necessary.
6. UpdateRadar: Polls the radar sensor for new data and updates the global radar-related variables.
7. ReadSensors: Reads the object and ambient temperatures from the MLX90614 sensor, validating the readings.
8. PublishData: Publishes the temperature and radar data to the MQTT broker.
9. EvaluateAlerts: Checks for various alert conditions (fever, hypothermia, apnea, bradypnea, tachypnea, bradycardia,     tachycardia) and sends an alert if any condition is met.

--SMTP Email--
SendEmail: Sends an alert email directly over SMTP/SMTPS, including the email headers, plain-text body, and HTML body.
The flow diagram provides a comprehensive overview of the system's functionality, including the setup process, the main loop that handles sensor data acquisition, processing, and communication, and the SMTP email sending mechanism for alert notifications.
The subgraphs and their detailed explanations help to clearly understand the purpose and implementation of each major component of the system. The diagram also highlights the key fixes and improvements made in the current version compared to the previous one, such as the I2S audio capture changes, MQTT reconnection handling, and the switch to WebSocket-based video streaming.
Overall, this detailed Mermaid flow diagram, along with the integrated code explanations, provides a valuable resource for understanding the structure, logic, and functionality of the Baby Vital Monitor firmware.

