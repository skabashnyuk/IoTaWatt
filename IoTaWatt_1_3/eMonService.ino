   /*******************************************************************************************************
 * eMonService - This SERVICE posts entries from the IotaLog to eMonCMS.  Details of the eMonCMS
 * account are provided in the configuration file at startup and this SERVICE is scheduled.  It runs
 * more or less independent of everything else, just reading the log records as they become available
 * and sending the data out.
 * The advantage of doing it this way is that there is really no eMonCMS specific code anywhere else
 * except a speciific section in getConfig.  Other web data logging services could be handled
 * the same way.
 * It's possible that multiple web services could be updated independently, each having their own 
 * SERVER.  The only issue right now would be the WiFi resource.  A future move to the 
 * asynchWifiClient would solve that.
 ******************************************************************************************************/
uint32_t eMonService(struct serviceBlock* _serviceBlock){
  // trace T_EMON
  enum   states {initialize, post, resend};
  static states state = initialize;
  static IotaLogRecord* logRecord = new IotaLogRecord;
  static double accum1Then [channels];
  static uint32_t UnixLastPost = UnixTime();
  static uint32_t UnixNextPost = UnixTime();
  static double _logHours;
  static String req = "";
  static uint32_t currentReqUnixtime = 0;
  static int  currentReqEntries = 0; 
  static uint32_t postTime = millis();

  trace(T_EMON,0);    
  switch(state){

    case initialize: {

          // We post the log to EmonCMS,
          // so wait until the log service is up and running.
      
      if(!iotaLog.isOpen()){
        return ((uint32_t) NTPtime() + 5);
      }
      msgLog("EmonCMS service started.");

          // Get the last record in the log.
          // Posting will begin with the next log entry after this one,
          // so in the future if there is a way to remember the last
          // that was posted (RTC memory or maybe use the SPIFFS) then 
          // that record should be loaded here.
          
      logRecord->UNIXtime = iotaLog.lastKey();
      iotaLog.readKey(logRecord);

          // Save the value*hrs to date, and logHours to date
      
      for(int i=0; i<channels; i++){ 
        accum1Then[i] = logRecord->channel[i].accum1;
      }
      _logHours = logRecord->logHours;

          // Assume that record was posted (not important).
          // Plan to start posting one interval later
      
      UnixLastPost = logRecord->UNIXtime;
      UnixNextPost = UnixLastPost + eMonCMSInterval - (UnixLastPost % eMonCMSInterval);

          // Advance state.
          // Set task priority low so that datalog will run before this.
      
      state = post;
      _serviceBlock->priority = priorityLow;
      return ((uint32_t) UnixNextPost + SEVENTY_YEAR_SECONDS);
    }

        
    case post: {

          // If we are current,
          // Anticipate next posting at next regular interval and break to reschedule.
 
      if(iotaLog.lastKey() < UnixNextPost){ 
        UnixNextPost = UnixTime() + eMonCMSInterval - (UnixTime() % eMonCMSInterval);
        return ((uint32_t)UnixNextPost + SEVENTY_YEAR_SECONDS);
      } 
      
          // Not current.  Read sequentially to get the entry >= scheduled post time
trace(T_EMON,1);    
      while(logRecord->UNIXtime < UnixNextPost){
        if(logRecord->UNIXtime >= iotaLog.lastKey()){
          msgLog("runaway seq read.", logRecord->UNIXtime);
          ESP.reset();
        }
        iotaLog.readNext(logRecord);
      }

          // Adjust the posting time to match the log entry time.
          // If new request, format preamble.
      
      UnixNextPost = logRecord->UNIXtime - logRecord->UNIXtime % eMonCMSInterval;
      if(req.length() == 0){
        req = "/input/bulk.json?time=" + String(UnixNextPost) + "&apikey=" + apiKey + "&data=[";
        currentReqUnixtime = UnixNextPost;
      }
      else {
        req += ',';
      }

          // Build the request string.
          // values for each channel are (delta value hrs)/(delta log hours) = period value.
          // Update the previous (Then) buckets to the most recent values.
     
trace(T_EMON,2);

      req += '[' + String(UnixNextPost - currentReqUnixtime) + ',' + String(node) + ',';
      double value1;
      double elapsedHours = logRecord->logHours - _logHours;
      _logHours = logRecord->logHours;
      for (int i = 0; i < channels; i++) {
        value1 = (logRecord->channel[i].accum1 - accum1Then[i]) / elapsedHours;
        accum1Then[i] = logRecord->channel[i].accum1;
        if(channelType[i] == channelTypeUndefined) req += "0,";
        else if(channelType[i] == channelTypeVoltage) req += String(value1,1) + ',';
        else if(channelType[i] == channelTypePower) req += String(long(value1+0.5)) + ',';
        else req += String(long(value1+0.5)) + ',';
      }
trace(T_EMON,3);    
      req.setCharAt(req.length()-1,']');
      currentReqEntries++;
      UnixLastPost = UnixNextPost;
      UnixNextPost +=  eMonCMSInterval - (UnixNextPost % eMonCMSInterval);
      
      if ((currentReqEntries < emonBulkEntries) ||
         ((iotaLog.lastKey() > UnixNextPost) &&
         (req.length() < 1000))) {
        return 1;
      }

          // Send the post       

      req += ']';
      uint32_t sendTime = millis();
      if(!eMonSend(req)){
        state = resend;
        return ((uint32_t)NTPtime() + 2);
      }
      Serial.print(formatHMS(NTPtime() + (localTimeDiff * 3600)));
      Serial.print(" ");
      Serial.print(millis()-sendTime);
      Serial.print(" ");
      Serial.println(req);
      req = "";
      currentReqEntries = 0;    
      state = post;
      
      return 1;
    }


    case resend: {
      msgLog("Resending eMonCMS data.");
      if(!eMonSend(req)){
        return ((uint32_t)NTPtime() + 5);
      }
      else {
        state = post;
        return ((uint32_t)NTPtime() + 1);
      }
      break;
    }
  }
  return ((uint32_t)NTPtime() + 1);
}

/************************************************************************************************
 *  eMonSend - send data to the eMonCMS server. 
 *  if secure transmission is configured, pas sthe request to a 
 *  similar WiFiClientSecure function.
 *  Secure takes about twice as long and can block sampling for more than a second.
 ***********************************************************************************************/
boolean eMonSend(String req){
  
  if(eMonSecure) return eMonSendSecure(req);
  trace(T_EMON,7);
  
  uint32_t startTime = millis();
  if(!WifiClient.connect(eMonURL.c_str(), 80)) {
        msgLog("failed to connect to:", eMonURL);
        WifiClient.stop();
        return false;
  } 
  yield();  
  WifiClient.println(String("GET ") + req);
  uint32_t _time = millis();
  while(WifiClient.available() < 2){
    yield();
    if((uint32_t)millis()-_time >= 200){
      msgLog("eMonCMS timeout.");
      WifiClient.stop();
      return false;
    }
  }
  yield();
  String reply = "";
  int maxlen = 40;
  while(WifiClient.available()){
    reply += (char)WifiClient.read();
    if(!maxlen--){
      break;
    }
  }
  if(reply.substring(0,2) != "ok"){
    msgLog("eMonCMS reply: ", reply);
    WifiClient.stop();
    return false;
  }
  WifiClient.stop();
//  Serial.print("Open Send ms: ");
//  Serial.println(millis()-startTime);
  return true;
}

boolean eMonSendSecure(String req){
  trace(T_EMON,8);
  ESP.wdtFeed();

      // Should always be disconnected, but test can't hurt.
    
  uint32_t startTime = millis();
  if(!WifiClientSecure.connected()){
    if(!WifiClientSecure.connect(eMonURL.c_str(), HttpsPort)) {
          msgLog("failed to connect to:",  eMonURL);
          WifiClientSecure.stop();
          return false;
    }
    if(!WifiClientSecure.verify(eMonSHA1,  eMonURL.c_str())){
      msgLog("eMonCMS could not validate certificate.");
      WifiClientSecure.stop();
      return false;
    }
  }
  yield();
  
      // Send the packet
   
  WifiClientSecure.print(String("GET ") + req + " HTTP/1.1\r\n" +
               "Host: " + eMonURL + "\r\n" +
               "User-Agent: IotaWatt\r\n" +
               "Connection: close\r\n\r\n"); 
 
      // Read through response header until blank line (\r\n)

  yield();    
  while (WifiClientSecure.connected()) {
    String line = WifiClientSecure.readStringUntil('\n');
    if (line == "\r") {
      break;
    }
  }

  yield(); 
  String line;
  while(WifiClientSecure.available()){
    line += char(WifiClientSecure.read());
  }
  if (!line.startsWith("ok")) {
    msgLog ("eMonCMS reply: ", line);
    WifiClientSecure.stop();
    return false;
  }              
  
//  Serial.print("Secure Send ms: ");
//  Serial.println(millis()-startTime);
  return true;
}


