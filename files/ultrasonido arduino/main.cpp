#include <Arduino.h>
#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include <timeLib.h>
#include <TM1638.h>

#define TRIGGER_PIN 6
#define ECHO_PIN 7
#define ENCODER_PIN 5
#define MOTOR_ENABLE 8
#define MOTOR_PWM 9

void run_trigger(void *pvParameters);
void readUltrasonic(void *pvParameters);
void run_timer(void *pvParameters);
void readEncoder(void *pvParameters);
void controlMotor(void *pvParameters);
void rpmencoder(void *pvParameters);
void display(void *pvParameters);
void encoderISR(void);
void isr_echo(void);
void isr_encoder(void);


//semaforo para sincronizar la lectura del echo
SemaphoreHandle_t sem1, sem2, mutex, enc, res, inicio;

long timer, encoder_timer, dis2;
bool echo_received = false, ena = false, arranque = false;
int dis = 0;
int encoder_count = 0;
TM1638 plac(12, 11, 10);
float rpmglobal = 0;
// Flag para permitir/deshabilitar el control automático del motor en tiempo de ejecución
volatile bool motor_control_enabled = true;
void setup() {
  Serial.begin(9600);
  pinMode(TRIGGER_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(ENCODER_PIN, INPUT_PULLUP);
  pinMode(MOTOR_ENABLE, OUTPUT);
  pinMode(MOTOR_PWM, OUTPUT);
  digitalWrite(MOTOR_ENABLE, HIGH);
  analogWriteResolution(16);
  analogWriteFrequency(1000); // Configurar la frecuencia a 1 kHz
  analogWrite(MOTOR_PWM, 0);
  plac.clearDisplay();
  plac.setBrightness(7);
  plac.clearLEDs();
  plac.displayNumber(0);
  plac.setLED(0, 1);
  sem1 = xSemaphoreCreateBinary();
  sem2 = xSemaphoreCreateBinary();
  inicio = xSemaphoreCreateBinary();
  mutex = xSemaphoreCreateMutex();
  enc = xSemaphoreCreateBinary();
  res = xSemaphoreCreateBinary();

  attachInterrupt(digitalPinToInterrupt(ECHO_PIN), isr_echo, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_PIN), isr_encoder, RISING);// Botón de reset conectado al pin B1
  
  xTaskCreate(run_trigger, "Trigger Task", 128, NULL, 4, NULL);
  xTaskCreate(readUltrasonic, "Read Ultrasonic Task", 128, NULL, 3, NULL);
  xTaskCreate(run_timer, "Run Timer", 128, NULL, 3, NULL);
  xTaskCreate(readEncoder, "Read Encoder Task", 128, NULL, 2, NULL);
  xTaskCreate(controlMotor, "Control Motor Task", 256, NULL, 4, NULL);
  xTaskCreate(rpmencoder, "RPM Encoder Task", 128, NULL, 1, NULL);
  xTaskCreate(display, "Display Task", 128, NULL, 3, NULL);
  vTaskStartScheduler();

}

void loop() {
  // Enviar pulso de trigger
  
}

void run_trigger(void *pvParameters)
{
  while(1)
  {
    //limpio semaforos para evitar que se acumulen
    digitalWrite(TRIGGER_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIGGER_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIGGER_PIN, LOW);
    vTaskDelay(50 / portTICK_PERIOD_MS); // Esperar 1 segundo antes de la siguiente medición
  }
}

void run_timer(void *pvParameters){
  while(1)
  {
    if((xSemaphoreTake(sem2, portMAX_DELAY) == pdTRUE) && !echo_received)
    {// Detener la interrupción para evitar interferencias
      echo_received = true;
      timer = micros();
       // Guardar el tiempo en que se recibe el pulso de echo// Volver a habilitar la interrupción para detectar el final del pulso de echo
    }
  }
}

void isr_echo(void)
{ 
  //liberar el semaforo para que la tarea de lectura pueda calcular la distancia
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  if (digitalRead(ECHO_PIN) == LOW) {
    //limpia semaforo para evitar que se acumulen
    xSemaphoreGiveFromISR(sem1, &xHigherPriorityTaskWoken);
  }
  else {
    xSemaphoreGiveFromISR(sem2, &xHigherPriorityTaskWoken);
  }
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void isr_encoder(void)
{
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  xSemaphoreGiveFromISR(enc, &xHigherPriorityTaskWoken);
}

void display(void *pvParameters)
{
  long local_dis;
  float local_rpm;
  while(1)
  {
    Serial.println("Actualizando display...");
    xSemaphoreTake(mutex, portMAX_DELAY);
    local_dis = dis;
    local_rpm = rpmglobal;
    xSemaphoreGive(mutex);
    plac.displayNumber(int(local_dis)+int(local_rpm)*10000);
    vTaskDelay(500 / portTICK_PERIOD_MS); // Actualizar cada 500 ms
  }
}    
void readEncoder(void *pvParameters)
{
  long last_interrupt_time = 0, interrupt_time, promedio = 0, intervalo = 0;
  int i = 0;
  while(1)
    {
      if(xSemaphoreTake(enc, portMAX_DELAY) == pdTRUE){
      interrupt_time = millis();
      promedio += (interrupt_time - last_interrupt_time);
      if (i < 10){
        i++;
      }
      else{
        promedio /= 10;
        intervalo = (0.1*promedio)+(0.9*intervalo);// Filtro de media móvil exponencial para suavizar la lectura
        i = 0;
        xSemaphoreTake(mutex, portMAX_DELAY);
        dis2 = intervalo;
        xSemaphoreGive(mutex);
      }
      last_interrupt_time = interrupt_time;
      xSemaphoreTake(mutex, portMAX_DELAY);
      encoder_count++;
      xSemaphoreGive(mutex);
    }
  }
  }
void controlMotor(void *pvParameters)
{
  int local_dis;
  float duty_cycle;
  uint16_t duty;
  while(1)
  {
    xSemaphoreTake(mutex, portMAX_DELAY);
    Serial.print(dis);
    local_dis = dis - 5;
    xSemaphoreGive(mutex);
    // map distance a duty (0..65535) y clausurar
    duty_cycle = local_dis * (65535.0) / 30.0;
    if (duty_cycle < 10000) duty_cycle = 0; //
    if (duty_cycle > (65535.0)) {
    duty_cycle = 65535.0;
    }
    if (duty_cycle != 0) duty = duty_cycle; // Agregar un pequeño margen para asegurar que el motor se mueva
    analogWrite(MOTOR_PWM, duty);
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}
void readUltrasonic(void *pvParameters)
{
  while(1)
  {
    if((xSemaphoreTake(sem1, portMAX_DELAY) == pdTRUE) && echo_received)
    {
      long duration = micros() - timer;
      echo_received = false; // Calcular la duración del pulso de echo
      long distance = (duration / 2) / 29.1;
      xSemaphoreTake(mutex, portMAX_DELAY);
       dis = int(distance);
      xSemaphoreGive(mutex);
      if (distance >= 400 || distance <= 2) {
        Serial.println("Out of range");
      } else {
      Serial.print("Distance: ");
      Serial.print(distance);
      Serial.println(" cm");
      }
    }
  }
}
void rpmencoder(void *pvParameters)
{
  int local_count;
  while(1)
  {
    xSemaphoreTake(mutex, portMAX_DELAY);
    local_count = encoder_count;
    encoder_count = 0;
    xSemaphoreGive(mutex);
    float rpm = (local_count / 20.0) * 6.0;
    xSemaphoreTake(mutex, portMAX_DELAY);
    rpmglobal = rpm;
    xSemaphoreGive(mutex);
    vTaskDelay(100 / portTICK_PERIOD_MS); // Actualizar cada 100 ms
  }
}