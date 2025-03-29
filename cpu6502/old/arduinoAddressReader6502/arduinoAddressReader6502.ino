#define               A0_PIN  A5
#define               A1_PIN  A4
#define               A2_PIN  A3
#define               A3_PIN  A2
#define               A4_PIN  A1
#define               A5_PIN  A0
#define               A6_PIN  13
#define               A7_PIN  12
#define               A8_PIN  11
#define               A9_PIN  10
#define               A10_PIN 9
#define               A11_PIN 8
#define               A12_PIN 7
#define               A13_PIN 6
#define               A14_PIN 5
#define               A15_PIN 4

#define READ_CLOCK_PIN 2 // interrupt

byte address[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

void setup() {
  // put your setup code here, to run once:
  pinMode(A0_PIN, INPUT);
  pinMode(A1_PIN, INPUT);
  pinMode(A2_PIN, INPUT);
  pinMode(A3_PIN, INPUT);
  pinMode(A4_PIN, INPUT);
  pinMode(A5_PIN, INPUT);
  pinMode(A6_PIN, INPUT);
  pinMode(A7_PIN, INPUT);
  pinMode(A8_PIN, INPUT);
  pinMode(A9_PIN, INPUT);
  pinMode(A10_PIN, INPUT);
  pinMode(A11_PIN, INPUT);
  pinMode(A12_PIN, INPUT);
  pinMode(A13_PIN, INPUT);
  pinMode(A14_PIN, INPUT);
  pinMode(A15_PIN, INPUT);

  pinMode(READ_CLOCK_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(READ_CLOCK_PIN), lookCpuClock, RISING);
  Serial.begin(115200);
}

void loop() {
  // put your main code here, to run repeatedly:

}

void lookCpuClock() {
  address[0]  = digitalRead(A0_PIN);
  address[1]  = digitalRead(A1_PIN);
  address[2]  = digitalRead(A2_PIN);
  address[3]  = digitalRead(A3_PIN);
  address[4]  = digitalRead(A4_PIN);
  address[5]  = digitalRead(A5_PIN);
  address[6]  = digitalRead(A6_PIN);
  address[7]  = digitalRead(A7_PIN);
  address[8]  = digitalRead(A8_PIN);
  address[9]  = digitalRead(A9_PIN);
  address[10] = digitalRead(A10_PIN);
  address[11] = digitalRead(A11_PIN);
  address[12] = digitalRead(A12_PIN);
  address[13] = digitalRead(A13_PIN);
  address[14] = digitalRead(A14_PIN);
  address[15] = digitalRead(A15_PIN);

  for(short i = 0; i < 16; i++) {
    Serial.print(address[i]);
  }

  Serial.println();
}