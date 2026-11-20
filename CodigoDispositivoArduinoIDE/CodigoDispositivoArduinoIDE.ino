#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>

bool verificarModemAT();
bool verificarSIM();
bool verificarRed();
bool enviarAT(const String& comando, const String& respuestaOK = "OK", unsigned long timeout = 3000);
void limpiarBufferModem();
bool enviarSMS(const String& mensaje, bool esEventoPrioritario = false, bool esPrueba = false);
void ejecutarEnvioTelemetria(const String& motivo);
String generarMensajeSMS(String tipoAlerta);

// configuración general

const char* WIFI_SSID = ""; //SSID del internet (nombre del wifi)
const char* WIFI_PASSWORD = ""; //contraseña

const int DEVICE_ID = 1; //identificación del dispositivo, cambia el número para ser otro dispositivo independiente

const char* SERVER_URL = "https://tip-proymf26.tipyenaccion.net/api/telemetry"; //url para el servidor
const char* API_KEY = ""; //api key (leer manual de instalación)


// MODEM A7670G
#define MODEM_RX 16
#define MODEM_TX 17

const int PIN_PWRKEY_MODEM = 4;

// Número obtenido dinámicamente desde el servidor
String NUMERO_DESTINO_SMS = "";

HardwareSerial modem(2);

bool modemOk = false;

// pines del esp32

const int PIN_DS18B20 = 21;
const int PIN_DETECTOR = 22;

const int LED_VERDE = 5;
const int LED_ROJO = 19;
const int PIN_BUZZER = 23;

// Lógica 4N25
// HIGH = Corte de corriente / operando en batería
// LOW  = Corriente electrica activa
const int VALOR_CORTE = HIGH;

// configuración del servidor

float temp_min_segura = -25.0;
float temp_max_segura = -10.0;

bool alerta_temperatura_activa = true;
bool alerta_bateria_activa = true;
bool alerta_vencimiento_activa = true;
bool alerta_modem_activa = true;
bool envio_sms_activo = true;

String plantilla_sms_cuerpo = "ALERTA FREEZERGUARD: Se detectó {tipo_alerta} en {dispositivo}. Temp: {temperatura} C."; //sms a enviar

int intervalo_telemetria = 5;

// protección para el sim, se ponen protecciones anti spam de mensajes asi evitamos que se bloquen.

unsigned long previoMillis = 0;
unsigned long previoBuzzerMillis = 0;
unsigned long previoImpresion = 0;
unsigned long previoLecturaTemp = 0;
unsigned long previoModemCheck = 0;

// Tiempo de espera de envío de SMS para alertas periódicas/temperatura
unsigned long ultimoEnvioSMS = 0;
const unsigned long COOLDOWN_SMS_MS = 60000; // 60 segundos mínimos entre SMS
const unsigned long INTERVALO_MODEM_CHECK_MS = 60000; // revisar el estado del módem cada 60s

// Tiempo de espera del detector 4N25 (2500ms para evitar falsos positivos)
unsigned long ultimoCambioDetector = 0;
int ultimoEstadoLectura = LOW;
const unsigned long TIEMPO_DEBOUNCE_CORTE_MS = 2500; // 2.5 segundos

bool estadoBuzzer = false;

// estado del dispositivo
float temperaturaActual = 0.0;
float ultimaLecturaValidaTemp = 0.0;
int lecturasFallidasTemp = 0;

bool enBateriaEnclavado = false;
bool estadoBateriaAnterior = false;
bool alertaTempSMSActiva = false; // Estado para controlar disparo de SMS por temperatura

// sensor de temperatura

OneWire oneWire(PIN_DS18B20);
DallasTemperature sensors(&oneWire);

//otras cosas del modem
void limpiarBufferModem() {
    while (modem.available()) {
        modem.read();
    }
}


bool esperarRespuesta(
    const String& respuestaOK,
    const String& respuestaError,
    unsigned long timeout
) {
    unsigned long inicio = millis();
    String respuesta = "";

    while (millis() - inicio < timeout) {
        while (modem.available()) {
            char c = modem.read();
            respuesta += c;
            Serial.write(c);

            if (respuesta.indexOf(respuestaOK) >= 0) {
                return true;
            }

            if (respuestaError.length() > 0 && respuesta.indexOf(respuestaError) >= 0) {
                return false;
            }
        }
        delay(5);
    }

    return false;
}

bool enviarAT(
    const String& comando,
    const String& respuestaOK,
    unsigned long timeout
) {
    limpiarBufferModem();

    Serial.print("[MODEM] >> ");
    Serial.println(comando);

    modem.println(comando);

    return esperarRespuesta(respuestaOK, "ERROR", timeout);
}

// generar sms de alerta segun lo recibido de la aplicación web

String generarMensajeSMS(String tipoAlerta) {
    if (plantilla_sms_cuerpo.length() == 0) {
        return "FREEZERGUARD: " + tipoAlerta + ". Temp: " + String(temperaturaActual, 1) + " C";
    }

    String msg = plantilla_sms_cuerpo;
    msg.replace("{tipo_alerta}", tipoAlerta);
    msg.replace("{dispositivo}", "Dispositivo #" + String(DEVICE_ID));
    msg.replace("{freezer}", "Freezer #" + String(DEVICE_ID));
    msg.replace("{temperatura}", String(temperaturaActual, 1));
    msg.replace("{fecha}", "Ahora");

    return msg;
}

//verificación del modem
bool verificarModemAT() {
    limpiarBufferModem();
    modem.println("AT");

    unsigned long inicio = millis();
    String respuesta = "";

    while (millis() - inicio < 2000) {
        while (modem.available()) {
            char c = modem.read();
            respuesta += c;

            if (respuesta.indexOf("OK") >= 0) {
                return true;
            }
        }
        delay(5);
    }

    return false;
}

//verificación del sim
bool verificarSIM() {
    limpiarBufferModem();
    modem.println("AT+CPIN?");

    unsigned long inicio = millis();
    String respuesta = "";

    while (millis() - inicio < 3000) {
        while (modem.available()) {
            char c = modem.read();
            respuesta += c;
            Serial.write(c);

            if (respuesta.indexOf("+CPIN: READY") >= 0) {
                return true;
            }

            if (respuesta.indexOf("ERROR") >= 0) {
                return false;
            }
        }
        delay(5);
    }

    return false;
}

//verificación de red
bool verificarRed() {
    limpiarBufferModem();
    modem.println("AT+CEREG?");

    unsigned long inicio = millis();
    String respuesta = "";

    while (millis() - inicio < 3000) {
        while (modem.available()) {
            char c = modem.read();
            respuesta += c;
            Serial.write(c);

            if (respuesta.indexOf("+CEREG: 0,1") >= 0 ||
                respuesta.indexOf("+CEREG: 1,1") >= 0 ||
                respuesta.indexOf("+CEREG: 0,5") >= 0 ||
                respuesta.indexOf("+CEREG: 1,5") >= 0) {
                return true;
            }
        }
        delay(5);
    }

    return false;
}

//Verificar estado del modem
void verificarSaludModem() {
    if (millis() - previoModemCheck < INTERVALO_MODEM_CHECK_MS && previoModemCheck != 0) {
        return;
    }
    previoModemCheck = millis();

    if (modemOk) {
        if (!verificarModemAT()) {
            Serial.println("[MODEM] Health Check: dejó de responder a AT.");
            modemOk = false;
        }
    } else {
        // Intento de auto-recuperación si vuelve a responder AT
        if (verificarModemAT() && verificarSIM()) {
            enviarAT("AT+CMGF=1");
            Serial.println("[MODEM] Health Check: Módem recuperado.");
            modemOk = true;
        }
    }
}

//iniciamos el modulo sms
bool inicializarModemSMS() {
    Serial.println();
    Serial.println("========================================");
    Serial.println("INICIALIZANDO MODEM A7670G");
    Serial.println("========================================");

    modem.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
    delay(500);

    // Encendido mediante pulso PWRKEY
    pinMode(PIN_PWRKEY_MODEM, OUTPUT);

    digitalWrite(PIN_PWRKEY_MODEM, HIGH);
    delay(100);

    digitalWrite(PIN_PWRKEY_MODEM, LOW);
    delay(1500);

    digitalWrite(PIN_PWRKEY_MODEM, HIGH);

    Serial.println("[MODEM] Esperando arranque...");
    delay(5000);

    bool conectado = false;

    for (int intento = 1; intento <= 5; intento++) {
        Serial.print("[MODEM] AT intento ");
        Serial.print(intento);
        Serial.println("/5");

        if (verificarModemAT()) {
            conectado = true;
            break;
        }

        delay(1000);
    }

    if (!conectado) {
        Serial.println("[MODEM] ERROR: no responde a AT.");
        modemOk = false;
        return false;
    }

    Serial.println("[MODEM] Comunicación AT OK.");

    Serial.println("[MODEM] Verificando SIM...");
    if (!verificarSIM()) {
        Serial.println("[MODEM] ERROR: SIM no disponible o bloqueada.");
        modemOk = false;
        return false;
    }
    Serial.println("[MODEM] SIM OK.");

    Serial.println("[MODEM] Verificando registro en red...");
    if (!verificarRed()) {
        Serial.println("[MODEM] ADVERTENCIA: aún no registrado en red celular.");
    } else {
        Serial.println("[MODEM] Red registrada.");
    }

    if (!enviarAT("AT+CMGF=1")) {
        Serial.println("[MODEM] ERROR configurando modo SMS (AT+CMGF=1).");
        modemOk = false;
        return false;
    }

    Serial.println("[MODEM] Modo SMS configurado.");
    modemOk = true;
    previoModemCheck = millis();
    Serial.println("[MODEM] Inicialización completada con éxito.");

    return true;
}

// enviar sms
bool enviarSMS(const String& mensaje, bool esEventoPrioritario, bool esPrueba) {
    if (!modemOk) {
        // Intento rápido de recuperación antes de cancelar
        if (verificarModemAT() && verificarSIM()) {
            enviarAT("AT+CMGF=1");
            modemOk = true;
        } else {
            Serial.println("[SMS] No se envía: módem no disponible.");
            return false;
        }
    }

    if (!envio_sms_activo && !esPrueba) {
        Serial.println("[SMS] No se envía: el envío de SMS está desactivado en el servidor.");
        return false;
    }

    if (NUMERO_DESTINO_SMS.length() < 3) {
        Serial.println("[SMS] No se envía: número de destino inválido o no recibido del servidor.");
        return false;
    }

    //para proteger el sim
    unsigned long ahora = millis();
    if (!esPrueba && !esEventoPrioritario && (ahora - ultimoEnvioSMS < COOLDOWN_SMS_MS) && ultimoEnvioSMS != 0) {
        Serial.print("[SMS] PROTECCIÓN SIM: Cooldown activo. Omitiendo envío. Debe esperar ");
        Serial.print((COOLDOWN_SMS_MS - (ahora - ultimoEnvioSMS)) / 1000);
        Serial.println(" segundos.");
        return false;
    }

    Serial.println();
    Serial.println("========================================");
    Serial.println("ENVIANDO SMS");
    Serial.println("========================================");
    Serial.print("[SMS] Destino: "); Serial.println(NUMERO_DESTINO_SMS);
    Serial.print("[SMS] Mensaje: "); Serial.println(mensaje);

    if (!verificarModemAT()) {
        Serial.println("[SMS] ERROR: módem no responde a AT.");
        modemOk = false;
        return false;
    }

    if (!enviarAT("AT+CMGF=1")) {
        Serial.println("[SMS] ERROR configurando modo texto.");
        modemOk = false;
        return false;
    }

    limpiarBufferModem();
    String comando = "AT+CMGS=\"" + NUMERO_DESTINO_SMS + "\"";
    Serial.print("[MODEM] >> "); Serial.println(comando);
    modem.println(comando);

    // Esperar prompt ">"
    unsigned long inicioPrompt = millis();
    bool promptRecibido = false;
    String respuestaPrompt = "";

    while (millis() - inicioPrompt < 5000) {
        while (modem.available()) {
            char c = modem.read();
            respuestaPrompt += c;
            Serial.write(c);

            if (c == '>') {
                promptRecibido = true;
                break;
            }

            if (respuestaPrompt.indexOf("ERROR") >= 0) {
                Serial.println();
                Serial.println("[SMS] ERROR al solicitar CMGS.");
                return false;
            }
        }

        if (promptRecibido) break;
        delay(5);
    }

    if (!promptRecibido) {
        Serial.println();
        Serial.println("[SMS] ERROR: no se recibió prompt '>'.");
        return false;
    }

    // Enviar contenido del mensaje
    modem.print(mensaje);
    delay(100);

    Serial.println();
    Serial.println("[SMS] Enviando Ctrl+Z (ASCII 26)...");
    modem.write(26);

    // Esperar confirmación
    unsigned long inicioConfirmacion = millis();
    String respuesta = "";

    while (millis() - inicioConfirmacion < 30000) {
        while (modem.available()) {
            char c = modem.read();
            respuesta += c;
            Serial.write(c);

            if (respuesta.indexOf("+CMGS:") >= 0) {
                // Actualizar cooldown solo cuando el envío fue verificado y exitoso por la red
                if (!esPrueba) {
                    ultimoEnvioSMS = millis();
                }
                Serial.println();
                Serial.println("[SMS] ¡SMS ENVIADO CORRECTAMENTE!");
                modemOk = true;
                return true;
            }

            if (respuesta.indexOf("ERROR") >= 0) {
                Serial.println();
                Serial.println("[SMS] ERROR enviando SMS.");
                return false;
            }
        }
        delay(5);
    }

    Serial.println();
    Serial.println("[SMS] TIMEOUT esperando confirmación de red.");
    return false;
}

//Sms de prueba al inicio para probar
void enviarSMSInicio() {
    String mensaje = "FREEZERGUARD: inicio correcto.\nDispositivo operativo.\nTemp: " + String(temperaturaActual, 1) + " C";
    enviarSMS(mensaje, false, false);
}

//sms de prueba cuando se da click a probar sms
void enviarSMSPrueba() {
    Serial.println();
    Serial.println("========================================");
    Serial.println("SMS DE PRUEBA");
    Serial.println("========================================");

    String mensaje = "FREEZERGUARD - PRUEBA DE SMS\nDispositivo: #" + String(DEVICE_ID) +
                     "\nTemp: " + String(temperaturaActual, 1) + " C\nEnergia: " +
                     (enBateriaEnclavado ? "BATERIA" : "RED") + "\nModem: " + (modemOk ? "OK" : "ERROR");

    enviarSMS(mensaje, false, true);
}

// Sms de corte de corriente y restauración

void enviarSMSAlertaCorte(bool hayCorte) {
    if (!alerta_bateria_activa) {
        Serial.println("[SMS] Alerta de batería desactivada por configuración local.");
        return;
    }

    String mensaje = "";
    if (hayCorte) {
        // Usa la plantilla configurada en la página web
        mensaje = generarMensajeSMS("CORTE DE CORRIENTE");
    } else {
        // Mensaje de confirmación
        mensaje = "FREEZERGUARD: CORRIENTE RESTAURADA.\nEl dispositivo vuelve a funcionar con red. Temp: " + String(temperaturaActual, 1) + " C";
    }

    // Permite enviar corte y restauración sin trabarse por cooldown
    enviarSMS(mensaje, true, false);
}

//alerta de temperatura por sms
void verificarAlertaTemperaturaSMS() {
    if (!alerta_temperatura_activa) {
        alertaTempSMSActiva = false;
        return;
    }

    bool fueraDeRango = (temperaturaActual < temp_min_segura || temperaturaActual > temp_max_segura);

    if (fueraDeRango != alertaTempSMSActiva) {
        alertaTempSMSActiva = fueraDeRango;

        String mensaje = "";
        if (fueraDeRango) {
            // usa la plantilla configurada en la página web
            mensaje = generarMensajeSMS("TEMPERATURA FUERA DE RANGO");
            enviarSMS(mensaje, false, false);
            ejecutarEnvioTelemetria("ALERTA_TEMPERATURA");
        } else {
            // Mensaje de confirmación
            mensaje = "FREEZERGUARD: TEMPERATURA RESTAURADA.\nLa temperatura volvio al rango seguro. Temp: " + String(temperaturaActual, 1) + " C";
            enviarSMS(mensaje, false, false);
            ejecutarEnvioTelemetria("TEMPERATURA_NORMALIZADA");
        }
    }
}

//conexión al internet
void conectarWiFi() {
    if (WiFi.status() == WL_CONNECTED) {
        return;
    }

    Serial.println();
    Serial.println("[WIFI] Conectando...");

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int intentos = 0;
    while (WiFi.status() != WL_CONNECTED && intentos < 25) {
        delay(500);
        Serial.print(".");
        intentos++;
    }

    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("[WIFI] Conectado. IP: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("[WIFI] No se pudo conectar.");
    }
}

// respuesta del servidor
void procesarRespuestaServidor(String respuesta) {
    Serial.println();
    Serial.println("========================================");
    Serial.println("RESPUESTA DEL SERVIDOR");
    Serial.println("========================================");
    Serial.println(respuesta);

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, respuesta);

    if (error) {
        Serial.print("[JSON] Error al deserializar: ");
        Serial.println(error.c_str());
        return;
    }

    // Configuración recibida
    JsonObject configuracion = doc["configuracion"];

    if (!configuracion.isNull()) {
        if (configuracion["temp_min"].is<float>() || configuracion["temp_min"].is<int>()) {
            temp_min_segura = configuracion["temp_min"];
        }

        if (configuracion["temp_max"].is<float>() || configuracion["temp_max"].is<int>()) {
            temp_max_segura = configuracion["temp_max"];
        }

        if (configuracion["alerta_temperatura_activa"].is<bool>()) {
            alerta_temperatura_activa = configuracion["alerta_temperatura_activa"];
        }

        if (configuracion["alerta_bateria_activa"].is<bool>()) {
            alerta_bateria_activa = configuracion["alerta_bateria_activa"];
        }

        if (configuracion["alerta_vencimiento_activa"].is<bool>()) {
            alerta_vencimiento_activa = configuracion["alerta_vencimiento_activa"];
        }

        if (configuracion["alerta_modem_activa"].is<bool>()) {
            alerta_modem_activa = configuracion["alerta_modem_activa"];
        }

        if (configuracion["envio_sms_activo"].is<bool>()) {
            envio_sms_activo = configuracion["envio_sms_activo"];
        }

        if (configuracion["intervalo_telemetria"].is<int>()) {
            int interval = configuracion["intervalo_telemetria"];
            if (interval > 0) {
                intervalo_telemetria = interval;
            }
        }

        if (!configuracion["plantilla_sms_cuerpo"].isNull()) {
            plantilla_sms_cuerpo = configuracion["plantilla_sms_cuerpo"].as<String>();
            plantilla_sms_cuerpo.trim();
        }

        if (!configuracion["telefonos_sms"].isNull()) {
            NUMERO_DESTINO_SMS = configuracion["telefonos_sms"].as<String>();
            NUMERO_DESTINO_SMS.trim();
            Serial.print("[SMS] Número obtenido del servidor: ");
            Serial.println(NUMERO_DESTINO_SMS);
        }

        // Detectar si la página web solicitó una prueba inmediata de SMS
        if (configuracion["solicitud_sms_prueba"].is<bool>() && configuracion["solicitud_sms_prueba"].as<bool>() == true) {
            Serial.println();
            Serial.println("[SMS] Solicitud de prueba de SMS recibida desde la interfaz web.");
            enviarSMSPrueba();
        }
    }

    Serial.println();
    Serial.println("[CONFIG] Valores sincronizados:");
    Serial.print("Temp mínima: "); Serial.println(temp_min_segura);
    Serial.print("Temp máxima: "); Serial.println(temp_max_segura);
    Serial.print("Alerta temperatura: "); Serial.println(alerta_temperatura_activa ? "ACTIVA" : "INACTIVA");
    Serial.print("Alerta batería: "); Serial.println(alerta_bateria_activa ? "ACTIVA" : "INACTIVA");
    Serial.print("Alerta módem: "); Serial.println(alerta_modem_activa ? "ACTIVA" : "INACTIVA");
    Serial.print("Envío SMS backend: "); Serial.println(envio_sms_activo ? "ACTIVO" : "INACTIVO");
    Serial.print("Plantilla SMS: "); Serial.println(plantilla_sms_cuerpo);
    Serial.print("Intervalo telemetría: "); Serial.print(intervalo_telemetria); Serial.println("s");
}


//enviamos datos al servidor
bool enviarTelemetriaNodeRed(String payload) {
    conectarWiFi();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[TELEMETRIA] Sin WiFi.");
        return false;
    }

    HTTPClient http;
    http.setTimeout(8000);

    Serial.println();
    Serial.println("[TELEMETRIA] Enviando datos...");

    http.begin(SERVER_URL);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-API-KEY", API_KEY);

    int codigo = http.POST(payload);

    Serial.print("[TELEMETRIA] HTTP: ");
    Serial.println(codigo);

    if (codigo > 0) {
        String respuesta = http.getString();
        procesarRespuestaServidor(respuesta);
        http.end();
        return codigo >= 200 && codigo < 300;
    }

    Serial.println("[TELEMETRIA] Error HTTP.");
    http.end();

    return false;
}

//mostramos los datos en la terminal de arduinoide
void ejecutarEnvioTelemetria(const String& motivo) {
    Serial.println();
    Serial.println("========================================");
    Serial.println("TELEMETRÍA");
    Serial.print("Motivo: "); Serial.println(motivo);
    Serial.println("========================================");

    Serial.print("Temperatura: "); Serial.print(temperaturaActual); Serial.println(" C");
    Serial.print("Energía: "); Serial.println(enBateriaEnclavado ? "BATERIA" : "RED");
    Serial.print("Módem: "); Serial.println(modemOk ? "OK" : "ERROR");

    // JSON Payload
    JsonDocument doc;
    doc["device_id"] = DEVICE_ID;
    doc["temperature"] = temperaturaActual;
    doc["bateria"] = enBateriaEnclavado;
    doc["modem_ok"] = modemOk;

    String payload;
    serializeJson(doc, payload);

    Serial.print("[TELEMETRIA] JSON: ");
    Serial.println(payload);

    enviarTelemetriaNodeRed(payload);
}


//sensor de temperatura
void leerTemperatura() {
    if (millis() - previoLecturaTemp < 1000 && previoLecturaTemp != 0) {
        return;
    }
    previoLecturaTemp = millis();

    sensors.requestTemperatures();
    float tempLeida = sensors.getTempCByIndex(0);

    // filtrar lecturas basura: -127°C (desconectado) o 85°C (no convertido) o valores imposibles
    if (tempLeida == DEVICE_DISCONNECTED_C || tempLeida == 85.0f || tempLeida < -50.0f || tempLeida > 100.0f) {
        lecturasFallidasTemp++;
        if (lecturasFallidasTemp % 10 == 0) {
            Serial.println("[TEMP] Ruido/Interrupción en bus DS18B20. Preservando última lectura válida.");
        }
        return; // Conserva la temperaturaActual anterior sin corromperla con picos falsos
    }

    // Filtro de saltos bruscos para evitar que salte de 20 grados a 0 de una
    if (temperaturaActual != 0.0 && abs(tempLeida - temperaturaActual) > 7.0f) {
        // Exige 2 lecturas consecutivas para validar un cambio real tan abrupto
        if (abs(tempLeida - ultimaLecturaValidaTemp) < 2.0f) {
            temperaturaActual = tempLeida;
        }
        ultimaLecturaValidaTemp = tempLeida;
        return;
    }

    // Lectura válida normal
    temperaturaActual = tempLeida;
    ultimaLecturaValidaTemp = tempLeida;
    lecturasFallidasTemp = 0;
}

// Detector de cortes de corriente 4n25

void procesarDetectorCorte() {
    int lectura = digitalRead(PIN_DETECTOR);

    // Si cambia el valor del pin, reiniciamos temporizador de filtro
    if (lectura != ultimoEstadoLectura) {
        ultimoCambioDetector = millis();
        ultimoEstadoLectura = lectura;
    }

    // Solo confirmamos el cambio si la lectura se mantiene estable durante 2.5 segundos
    if ((millis() - ultimoCambioDetector) >= TIEMPO_DEBOUNCE_CORTE_MS) {
        // HIGH = Corte de energía / Batería
        // LOW  = Red eléctrica activa
        enBateriaEnclavado = (lectura == VALOR_CORTE);
    }
}

// alarmas fisicas (leds y buzzer)
void procesarAlarmasFisicas() {
    bool alarmaTemperatura = alerta_temperatura_activa && (temperaturaActual < temp_min_segura || temperaturaActual > temp_max_segura);
    bool alarmaBateria = alerta_bateria_activa && enBateriaEnclavado;
    bool alarmaModem = alerta_modem_activa && !modemOk;

    bool alarmaGeneral = alarmaTemperatura || alarmaBateria || alarmaModem;

    if (alarmaGeneral) {
        digitalWrite(LED_ROJO, HIGH);
        digitalWrite(LED_VERDE, LOW);

        if (millis() - previoBuzzerMillis >= 500) {
            previoBuzzerMillis = millis();
            estadoBuzzer = !estadoBuzzer;
            if (estadoBuzzer) {
                tone(PIN_BUZZER, 2000); // Emite tono de 2000 Hz
            } else {
                noTone(PIN_BUZZER);
            }
        }
    } else {
        digitalWrite(LED_ROJO, LOW);
        digitalWrite(LED_VERDE, HIGH);
        noTone(PIN_BUZZER);
        digitalWrite(PIN_BUZZER, LOW);
        estadoBuzzer = false;
    }
}

//procesamos comandos en el monitor serial
void procesarComandosSerial() {
    if (!Serial.available()) {
        return;
    }

    String comando = Serial.readStringUntil('\n');
    comando.trim();

    if (comando.equalsIgnoreCase("SMSPRUEBA")) {
        enviarSMSPrueba();
    } else {
        Serial.print("[SERIAL] Comando desconocido: ");
        Serial.println(comando);
    }
}

//iniciamos todo
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("========================================");
    Serial.println("       FREEZERGUARD ESP32");
    Serial.println("========================================");

    // Pines
    pinMode(LED_VERDE, OUTPUT);
    pinMode(LED_ROJO, OUTPUT);
    pinMode(PIN_BUZZER, OUTPUT);
    pinMode(PIN_DETECTOR, INPUT_PULLUP);

    digitalWrite(LED_VERDE, LOW);
    digitalWrite(LED_ROJO, LOW);
    noTone(PIN_BUZZER);
    digitalWrite(PIN_BUZZER, LOW);

    // Sensor de temperatura
    sensors.begin();
    sensors.requestTemperatures();
    float tempIni = sensors.getTempCByIndex(0);
    if (tempIni != DEVICE_DISCONNECTED_C && tempIni != 85.0f) {
        temperaturaActual = tempIni;
        ultimaLecturaValidaTemp = tempIni;
    }

    Serial.print("[TEMP] Temperatura inicial: ");
    Serial.print(temperaturaActual);
    Serial.println(" C");

    // Detector 4N25
    int lecturaIni = digitalRead(PIN_DETECTOR);
    ultimoEstadoLectura = lecturaIni;
    enBateriaEnclavado = (lecturaIni == VALOR_CORTE);
    estadoBateriaAnterior = enBateriaEnclavado;

    Serial.print("[ENERGIA] Estado inicial: ");
    Serial.println(enBateriaEnclavado ? "BATERIA / CORTE" : "RED");

    // Módem A7670G
    inicializarModemSMS();

    // WiFi
    conectarWiFi();

    // Primera telemetría
    ejecutarEnvioTelemetria("INICIO");

    // SMS de inicio
    if (modemOk && NUMERO_DESTINO_SMS.length() >= 3 && envio_sms_activo) {
        enviarSMSInicio();
    } else {
        Serial.println();
        Serial.println("[SMS] No se envía SMS de inicio.");
        if (!modemOk) Serial.println("[SMS] Motivo: módem no disponible.");
        if (!envio_sms_activo) Serial.println("[SMS] Motivo: envío de SMS desactivado en servidor.");
        if (NUMERO_DESTINO_SMS.length() < 3) Serial.println("[SMS] Motivo: no hay número válido configurado.");
    }

    Serial.println();
    Serial.println("========================================");
    Serial.println("FREEZERGUARD INICIADO");
    Serial.println("========================================");
    Serial.println("Comando serial: SMSPRUEBA");

    previoMillis = millis();
    previoImpresion = millis();
}


//loop principal
void loop() {
    // Comandos Serial
    procesarComandosSerial();

    // Temperatura
    leerTemperatura();

    // Detector de corte 4N25
    procesarDetectorCorte();

    // Alarmas físicas
    procesarAlarmasFisicas();

    // Health Check periódico del módem
    verificarSaludModem();

    // Monitorear disparo de SMS por Temperatura Fuera de Rango
    verificarAlertaTemperaturaSMS();

    // Detectar cambio de estado de energía
    if (enBateriaEnclavado != estadoBateriaAnterior) {
        if (enBateriaEnclavado) {
            Serial.println();
            Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
            Serial.println("⚠ CORTE DE CORRIENTE DETECTADO");
            Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

            // SMS de alerta por corte
            enviarSMSAlertaCorte(true);

            // Telemetría inmediata al servidor
            ejecutarEnvioTelemetria("CORTE");
        } else {
            Serial.println();
            Serial.println("========================================");
            Serial.println("✓ CORRIENTE RESTAURADA");
            Serial.println("========================================");

            // SMS de notificación por restauración
            enviarSMSAlertaCorte(false);

            // Telemetría inmediata al servidor
            ejecutarEnvioTelemetria("RESTAURACION");
        }

        estadoBateriaAnterior = enBateriaEnclavado;
    }

    // Monitoreo por consola
    if (millis() - previoImpresion >= 1000) {
        previoImpresion = millis();

        Serial.print("[ESTADO] Temp: ");
        Serial.print(temperaturaActual, 1);
        Serial.print(" C | Energía: ");
        Serial.print(enBateriaEnclavado ? "BATERIA" : "RED");
        Serial.print(" | Módem: ");
        Serial.println(modemOk ? "OK" : "ERROR");
    }

    // Telemetría periódica programada
    unsigned long intervaloMs = (unsigned long)(intervalo_telemetria > 0 ? intervalo_telemetria : 5) * 1000UL;

    if (millis() - previoMillis >= intervaloMs) {
        previoMillis = millis();
        ejecutarEnvioTelemetria("PERIODICA");
    }

    delay(10);
}
