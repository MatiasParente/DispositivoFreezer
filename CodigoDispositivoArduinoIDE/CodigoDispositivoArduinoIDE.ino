#include <Arduino.h>
#include <math.h>

// 1. Definición de Pines (Se mantienen los estables que elegimos)
const int PIN_NTC = 34;
const int LED_VERDE = 18;
const int LED_ROJO = 19;
const int PIN_BUZZER = 23;

// Constante para el cálculo del termistor NTC
const float BETA = 3950;

// Variables de control para las alertas en el Monitor Serie
bool alertaTemperaturaEnviada = false;

void setup() {
  // Inicializamos el puerto serie para ver las lecturas en la compu
  Serial.begin(115200);

  // Configuración de pines como salidas
  pinMode(LED_VERDE, OUTPUT);
  pinMode(LED_ROJO, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  
  // Estado inicial: Todo normal (Verde encendido, Alaras apagadas)
  digitalWrite(LED_VERDE, HIGH);
  digitalWrite(LED_ROJO, LOW);
  digitalWrite(PIN_BUZZER, LOW);

  Serial.println("--- Sistema de Monitoreo de Freezer Listo ---");
}

void loop() {
  // 2. Lectura analógica y cálculo de la Temperatura (Celsius)
  int lectura = analogRead(PIN_NTC);

  // Evitamos indeterminaciones matemáticas (divisiones por cero o logaritmos negativos)
  if (lectura == 0) lectura = 1;
  if (lectura >= 4095) lectura = 4094;

  // Ecuación de Steinhart-Hart simplificada usando la constante BETA
  float temperatura = 1.0 / (log(1.0 / (4095.0 / lectura - 1.0)) / BETA + 1.0 / 298.15) - 273.15;

  // Imprimimos el valor actual por el monitor serie
  Serial.print("Temperatura Actual: ");
  Serial.print(temperatura, 1); // Muestra 1 decimal
  Serial.println(" °C");

  // 3. Lógica de Alerta por Alta Temperatura (> 30 °C)
  if (temperatura > 30.0) {
    // ESTADO DE ALERTA: Prendemos Rojo, apagamos Verde
    digitalWrite(LED_ROJO, HIGH);
    digitalWrite(LED_VERDE, LOW);
    
    // Pitido intermitente corto del Buzzer
    tone(PIN_BUZZER, 1000);
    delay(200);
    noTone(PIN_BUZZER);

    // Mensaje único en consola cuando cruza el límite (Simula el envío de datos)
    if (!alertaTemperaturaEnviada) {
      Serial.println("[ALERTA CRÍTICA] ¡La temperatura superó los 30°C!");
      alertaTemperaturaEnviada = true;
    }
  } 
  else {
    // ESTADO NORMAL: Prendemos Verde, apagamos Rojo y silenciamos Buzzer
    digitalWrite(LED_ROJO, LOW);
    digitalWrite(LED_VERDE, HIGH);
    noTone(PIN_BUZZER);
    
    // Reseteamos el control cuando la temperatura vuelve a la normalidad
    alertaTemperaturaEnviada = false;
  }

  // Espera de 500ms entre lecturas para mantener estable el ADC
  delay(500); 
}