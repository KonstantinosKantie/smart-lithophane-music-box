#include <EEPROM.h>
#include <SoftwareSerial.h>

SoftwareSerial mySerial(10, 11); // RX, TX for DFPlayer

// DFPlayer serial packet framing
#define Start_Byte 0x7E
#define Version_Byte 0xFF
#define Command_Length 0x06
#define End_Byte 0xEF
#define Acknowledge 0x00

// Button pins (active LOW with internal pull-ups)
const int buttonNext = 2;
const int buttonPause = 3;
const int buttonPrevious = 4;
const int buttonFolder = 5;
const int buttonFavorite = 6;

// Playback state
boolean isPlaying = false;
boolean isFavMode = false;
int volume_now = 20;
int folder_now = 1;
int song_now = 1;

// Folder setup: index 0 is unused since DFPlayer folders start at 01
int folder_total = 2;
int tracksPerFolder[] = {0, 15, 20}; // Folder 1: 15 tracks, Folder 2: 20 tracks

// Usage tracker: counts how many times the box runs for >10 minutes
unsigned long sessionThreshold = 600000;
boolean hasCountedSession = false;
unsigned long bootCount = 0;
const int mem_BootCount = 200;

// EEPROM memory map
const int mem_Volume = 0;
const int mem_LastFolder = 1;
const int mem_LastTrack = 2;
const int mem_SaveIdx = 3;
const int BASE_FAV_ADDRESS = 10;
const int MAX_FAVS = 5;

struct SongInfo {
  byte folder;
  byte track;
};

SongInfo myFavorites[MAX_FAVS];
int favSaveIndex = 0;
int favPlayIndex = 0;

void setup() {
  pinMode(buttonPause, INPUT_PULLUP);
  pinMode(buttonNext, INPUT_PULLUP);
  pinMode(buttonPrevious, INPUT_PULLUP);
  pinMode(buttonFolder, INPUT_PULLUP);
  pinMode(buttonFavorite, INPUT_PULLUP);

  mySerial.begin(9600);
  delay(1000);
  Serial.begin(9600);
  delay(1000);

  // Load 10-minute session counter (handle uninitialized EEPROM)
  EEPROM.get(mem_BootCount, bootCount);
  if (bootCount == 0xFFFFFFFFUL) {
    bootCount = 0;
    EEPROM.put(mem_BootCount, bootCount);
  }
  Serial.print("--- Sessions over 10 mins: ");
  Serial.print(bootCount);
  Serial.println(" ---");

  // Load and print saved favorites for debugging
  Serial.println("--- DFPlayer Favorites List ---");
  EEPROM.get(BASE_FAV_ADDRESS, myFavorites);
  
  for (int i = 0; i < MAX_FAVS; i++) {
    Serial.print("Slot ");
    Serial.print(i);
    Serial.print(": ");
    if (myFavorites[i].folder == 255) {
      Serial.println("EMPTY");
    } else {
      Serial.print("Folder ");
      Serial.print(myFavorites[i].folder);
      Serial.print(" | Track ");
      Serial.println(myFavorites[i].track);
    }
  }
  Serial.println("-------------------------------");

  // Restore last volume, folder, and track with sanity checks
  volume_now = EEPROM.read(mem_Volume);
  if (volume_now > 30) volume_now = 20;

  folder_now = EEPROM.read(mem_LastFolder);
  if (folder_now == 255 || folder_now == 0 || folder_now > folder_total) folder_now = 1;

  song_now = EEPROM.read(mem_LastTrack);
  if (song_now == 255 || song_now == 0 || song_now > tracksPerFolder[folder_now]) song_now = 1;

  favSaveIndex = EEPROM.read(mem_SaveIdx);
  if (favSaveIndex >= MAX_FAVS) favSaveIndex = 0;

  // Start playback from last saved state
  setVolume(volume_now);
  delay(500);
  execute_CMD(0x0F, folder_now, song_now);
  isPlaying = true;
}

void loop() {
  // Listen for DFPlayer feedback: 0x3D means current track finished
  if (mySerial.available() >= 10) {
    if (mySerial.read() == 0x7E) {
      byte buf[9];
      mySerial.readBytes((char*)buf, 9);
      if (buf[2] == 0x3D) {
        Serial.println("Track finished. Auto-playing next...");
        playNext();
      }
    }
  }

  // Log a valid session once uptime passes 10 minutes
  if (!hasCountedSession && millis() >= sessionThreshold) {
    hasCountedSession = true;
    if (bootCount < 0xFFFFFFFEUL) {
      bootCount++;
      EEPROM.put(mem_BootCount, bootCount);
      Serial.print("10 minutes reached! Total valid sessions: ");
      Serial.println(bootCount);
    }
  }

  // 1. Pause / Play
  if (digitalRead(buttonPause) == LOW) {
    if (isPlaying) {
      execute_CMD(0x0E, 0, 0);
      isPlaying = false;
    } else {
      execute_CMD(0x0D, 0, 1);
      isPlaying = true;
    }
    delay(400);
  }

  // 2. Next track (short press) / Volume Up (hold > 500ms)
  if (digitalRead(buttonNext) == LOW) {
    unsigned long startHold = millis();
    bool isHold = false;
    while (digitalRead(buttonNext) == LOW) {
      if (millis() - startHold > 500) {
        isHold = true;
        volume_now = min(volume_now + 1, 30);
        setVolume(volume_now);
        delay(150);
      }
    }
    if (isHold) EEPROM.update(mem_Volume, volume_now);
    else playNext();
    
    delay(200);
  }

  // 3. Previous track (short press) / Volume Down (hold > 500ms)
  if (digitalRead(buttonPrevious) == LOW) {
    unsigned long startHold = millis();
    bool isHold = false;
    while (digitalRead(buttonPrevious) == LOW) {
      if (millis() - startHold > 500) {
        isHold = true;
        volume_now = max(volume_now - 1, 0);
        setVolume(volume_now);
        delay(150);
      }
    }
    if (isHold) EEPROM.update(mem_Volume, volume_now);
    else playPrevious();
    
    delay(200);
  }

  // 4. Switch playlist / folder
  if (digitalRead(buttonFolder) == LOW) {
    isFavMode = false;
    folder_now++;
    if (folder_now > folder_total) folder_now = 1;
    song_now = 1;
    execute_CMD(0x0F, folder_now, song_now);
    saveCurrentState();
    delay(400);
  }

  // 5. Favorites: short press = play, >5s = save current track, >15s = wipe all
  if (digitalRead(buttonFavorite) == LOW) {
    unsigned long startTime = millis();
    while (digitalRead(buttonFavorite) == LOW);
    unsigned long duration = millis() - startTime;

    if (duration > 15000) {
      for (int i = 0; i < MAX_FAVS; i++) {
        myFavorites[i].folder = 255;
        myFavorites[i].track = 255;
      }
      EEPROM.put(BASE_FAV_ADDRESS, myFavorites);
      favSaveIndex = 0;
      EEPROM.update(mem_SaveIdx, 0);
      Serial.println("FAVORITES WIPED");
    }
    else if (duration > 5000) {
      myFavorites[favSaveIndex].folder = folder_now;
      myFavorites[favSaveIndex].track = song_now;
      EEPROM.put(BASE_FAV_ADDRESS, myFavorites);
      Serial.print("SAVED to Slot "); Serial.print(favSaveIndex);
      Serial.print(": F"); Serial.print(folder_now); Serial.print(" T"); Serial.println(song_now);
      
      favSaveIndex = (favSaveIndex + 1) % MAX_FAVS;
      EEPROM.update(mem_SaveIdx, favSaveIndex);

      // Brief pause/resume as audio confirmation
      execute_CMD(0x0E, 0, 0);
      delay(200);
      execute_CMD(0x0D, 0, 1);
    }
    else if (duration > 50) {
      isFavMode = true;
      bool found = false;
      for (int i = 0; i < MAX_FAVS; i++) {
        int checkIdx = (favPlayIndex + i) % MAX_FAVS;
        if (myFavorites[checkIdx].folder != 255 && myFavorites[checkIdx].folder != 0) {
          favPlayIndex = checkIdx;
          Serial.print("PLAYING FAV SLOT: "); Serial.println(favPlayIndex);
          playFavorite(favPlayIndex);
          favPlayIndex = (favPlayIndex + 1) % MAX_FAVS;
          found = true;
          break;
        }
      }
      if (!found) {
        Serial.println("NO FAVORITES FOUND TO PLAY");
        isFavMode = false;
      }
    }
    delay(200);
  }
}

// ==========================================
// Helper Functions
// ==========================================

void playFavorite(int index) {
  byte f = myFavorites[index].folder;
  byte t = myFavorites[index].track;

  if (f != 255 && f != 0) {
    folder_now = f;
    song_now = t;
    execute_CMD(0x0F, folder_now, song_now);
    saveCurrentState();
  }
}

void playNext() {
  // Wrap back to track 1 after the last track in the folder
  if (song_now >= tracksPerFolder[folder_now]) {
    song_now = 1;
  } else {
    song_now++;
  }
  execute_CMD(0x0F, folder_now, song_now);
  saveCurrentState();
  delay(200);
}

void playPrevious() {
  // Wrap to the last track if going back from track 1
  if (song_now <= 1) {
    song_now = tracksPerFolder[folder_now];
  } else {
    song_now--;
  }
  execute_CMD(0x0F, folder_now, song_now);
  saveCurrentState();
  delay(200);
}

void setVolume(int vol) {
  execute_CMD(0x06, 0, vol);
}

void saveCurrentState() {
  EEPROM.update(mem_LastFolder, folder_now);
  EEPROM.update(mem_LastTrack, song_now);
}

// Builds and sends a raw 10-byte DFPlayer command frame with checksum
void execute_CMD(byte CMD, byte Par1, byte Par2) {
  word checksum = -(Version_Byte + Command_Length + CMD + Acknowledge + Par1 + Par2);
  byte Command_line[10] = {
    Start_Byte, Version_Byte, Command_Length, CMD, Acknowledge,
    Par1, Par2, highByte(checksum), lowByte(checksum), End_Byte
  };
  for (byte k = 0; k < 10; k++) {
    mySerial.write(Command_line[k]);
  }
}