/*
 * ESPWebThingAdapter.h V3.10.0
 *
 * Exposes the Web Thing API based on provided ThingDevices.
 * Suitable for ESP32 and ESP8266 using ESPAsyncWebServer and ESPAsyncTCP
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 * 
 */

/*
A couple of things worth watching during testing/deployment, given what's in this file:

Body size limit: ESP_MAX_PUT_BODY_SIZE is 512 bytes. If any PUT/POST payload (property updates, action requests) gets close to that, you'll hit the 413 response added in handleBody. Worth a quick check with a realistic payload size.

_tempObject body buffer lifecycle: since BodyData is malloc'd per-request and freed via freeBodyData, it's worth confirming under concurrent/rapid requests (e.g. a controller hammering property updates) that nothing leaks — a quick heap-watch (ESP.getFreeHeap()) during a stress test would catch it early.

WebSocket reconnects: if you're using the /things/{id} websocket for property change notifications, worth testing a client disconnect/reconnect cycle to make sure removeEventSubscriptions behaves as expected.
*/

#pragma once

#ifdef ESP32
#include <AsyncTCP.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#include <ESP8266mDNS.h>
#endif

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include "Thing.h"

#define ESP_MAX_PUT_BODY_SIZE 512

class WebThingAdapter {
public:
  WebThingAdapter(const String &_name, IPAddress _ip, uint16_t _port = 80,
                  bool _disableHostValidation = false)
      : server(_port), name(_name), ip(_ip.toString()), port(_port),
        disableHostValidation(_disableHostValidation) {}

  void begin() {
    name.toLowerCase();
    if (MDNS.begin(this->name.c_str())) {
      Serial.println("MDNS responder started");
    }

    MDNS.addService("webthing", "tcp", port);
    MDNS.addServiceTxt("webthing", "tcp", "path", "/");

    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods",
                                         "GET, POST, PUT, DELETE, OPTIONS");
    DefaultHeaders::Instance().addHeader(
        "Access-Control-Allow-Headers",
        "Origin, X-Requested-With, Content-Type, Accept");

    this->server.onNotFound((ArRequestHandlerFunction)std::bind(&WebThingAdapter::handleUnknown, this,
                                      std::placeholders::_1));

    this->server.on("/*", HTTP_OPTIONS,
                    (ArRequestHandlerFunction)std::bind(&WebThingAdapter::handleOptions, this,
                              std::placeholders::_1));
							  
    this->server.on("/", HTTP_GET,
                    (ArRequestHandlerFunction)std::bind(&WebThingAdapter::handleThings, this,
                              std::placeholders::_1));

    ThingDevice *device = this->firstDevice;
    while (device != nullptr) {
      String deviceBase = "/things/" + device->id;

      ThingProperty *property = device->firstProperty;
      while (property != nullptr) {
        String propertyBase = deviceBase + "/properties/" + property->id;
        this->server.on(propertyBase.c_str(), HTTP_GET,
                        (ArRequestHandlerFunction)std::bind(&WebThingAdapter::handleThingPropertyGet,
                                  this, std::placeholders::_1, property));
        this->server.on(propertyBase.c_str(), HTTP_PUT,
                        (ArRequestHandlerFunction)std::bind(&WebThingAdapter::handleThingPropertyPut,
                                  this, std::placeholders::_1, device,
                                  property),
                        NULL,
                        (ArBodyHandlerFunction)std::bind(&WebThingAdapter::handleBody, this,
                                  std::placeholders::_1, std::placeholders::_2,
                                  std::placeholders::_3, std::placeholders::_4,
                                  std::placeholders::_5));

        property = (ThingProperty *)property->next;
      }

      ThingAction *action = device->firstAction;
      while (action != nullptr) {
        String actionBase = deviceBase + "/actions/" + action->id;
        this->server.on(actionBase.c_str(), HTTP_GET,
                        (ArRequestHandlerFunction)std::bind(&WebThingAdapter::handleThingActionGet, this,
                                  std::placeholders::_1, device, action));
        this->server.on(actionBase.c_str(), HTTP_POST,
                        (ArRequestHandlerFunction)std::bind(&WebThingAdapter::handleThingActionPost,
                                  this, std::placeholders::_1, device, action),
                        NULL,
                        (ArBodyHandlerFunction)std::bind(&WebThingAdapter::handleBody, this,
                                  std::placeholders::_1, std::placeholders::_2,
                                  std::placeholders::_3, std::placeholders::_4,
                                  std::placeholders::_5));
        this->server.on(actionBase.c_str(), HTTP_DELETE,
                        (ArRequestHandlerFunction)std::bind(&WebThingAdapter::handleThingActionDelete,
                                  this, std::placeholders::_1, device,
                                  action));
        action = (ThingAction *)action->next;
      }

      ThingEvent *event = device->firstEvent;
      while (event != nullptr) {
        String eventBase = deviceBase + "/events/" + event->id;
        this->server.on(eventBase.c_str(), HTTP_GET,
                        (ArRequestHandlerFunction)std::bind(&WebThingAdapter::handleThingEventGet, this,
                                  std::placeholders::_1, device, event));
        event = (ThingEvent *)event->next;
      }

      this->server.on((deviceBase + "/properties").c_str(), HTTP_GET,
                      (ArRequestHandlerFunction)std::bind(&WebThingAdapter::handleThingPropertiesGet,
                                this, std::placeholders::_1,
                                device->firstProperty));
      this->server.on((deviceBase + "/actions").c_str(), HTTP_GET,
                      (ArRequestHandlerFunction)std::bind(&WebThingAdapter::handleThingActionsGet, this,
                                std::placeholders::_1, device));
      this->server.on((deviceBase + "/actions").c_str(), HTTP_POST,
                      (ArRequestHandlerFunction)std::bind(&WebThingAdapter::handleThingActionsPost, this,
                                std::placeholders::_1, device),
                      NULL,
                      (ArBodyHandlerFunction)std::bind(&WebThingAdapter::handleBody, this,
                                std::placeholders::_1, std::placeholders::_2,
                                std::placeholders::_3, std::placeholders::_4,
                                std::placeholders::_5));
      this->server.on((deviceBase + "/events").c_str(), HTTP_GET,
                      (ArRequestHandlerFunction)std::bind(&WebThingAdapter::handleThingEventsGet, this,
                                std::placeholders::_1, device));
      this->server.on(deviceBase.c_str(), HTTP_GET,
                      (ArRequestHandlerFunction)std::bind(&WebThingAdapter::handleThing, this,
                                std::placeholders::_1, device));

      device = device->next;
    }

    this->server.begin();
  }

  void update() {
#ifdef ESP8266
    MDNS.update();
#endif
#ifndef WITHOUT_WS
    // * Send changed properties as defined in "4.5 propertyStatus message"
    // Do this by looping over all devices and properties
    ThingDevice *device = this->firstDevice;
    while (device != nullptr) {
      sendChangedProperties(device);
      device = device->next;
    }
#endif
  }

  void addDevice(ThingDevice *device) {
    if (this->lastDevice == nullptr) {
      this->firstDevice = device;
      this->lastDevice = device;
    } else {
      this->lastDevice->next = device;
      this->lastDevice = device;
    }

#ifndef WITHOUT_WS
    // Initiate the websocket instance
    AsyncWebSocket *ws = new AsyncWebSocket("/things/" + device->id);
    device->ws = ws;
    // AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType
    // type, void * arg, uint8_t *data, size_t len, ThingDevice* device
    ws->onEvent(std::bind(
        &WebThingAdapter::handleWS, this, std::placeholders::_1,
        std::placeholders::_2, std::placeholders::_3, std::placeholders::_4,
        std::placeholders::_5, std::placeholders::_6, device));
    this->server.addHandler(ws);
#endif
  }

private:
  // Per-request PUT/POST body buffer.
  //
  // This is stashed on AsyncWebServerRequest::_tempObject rather than on
  // the adapter itself, since ESPAsyncWebServer can interleave body
  // callbacks for multiple simultaneously-connected clients. A single
  // shared buffer on the adapter would let one client's request corrupt
  // another's in-flight body. _tempObject is a void* the framework
  // reserves for exactly this purpose and frees with free() when the
  // request is destroyed, so we allocate it with malloc().
  struct BodyData {
    char data[ESP_MAX_PUT_BODY_SIZE];
    size_t len;
    bool complete;
  };

  AsyncWebServer server;

  String name;
  String ip;
  uint16_t port;
  bool disableHostValidation;
  ThingDevice *firstDevice = nullptr;
  ThingDevice *lastDevice = nullptr;

  static BodyData *getBodyData(AsyncWebServerRequest *request) {
    return reinterpret_cast<BodyData *>(request->_tempObject);
  }

  static void freeBodyData(AsyncWebServerRequest *request) {
    if (request->_tempObject != nullptr) {
      free(request->_tempObject);
      request->_tempObject = nullptr;
    }
  }

  bool verifyHost(AsyncWebServerRequest *request) {
    if (disableHostValidation) {
      return true;
    }

    const AsyncWebHeader *header = request->getHeader("Host");
    if (header == nullptr) {
      request->send(403);
      return false;
    }
    String value = header->value();
    int colonIndex = value.indexOf(':');
    if (colonIndex >= 0) {
      value.remove(colonIndex);
    }
    if (value.equalsIgnoreCase(name + ".local") || value == ip ||
        value == "localhost") {
      return true;
    }
    request->send(403);
    return false;
  }

#ifndef WITHOUT_WS
  void sendErrorMsg(JsonDocument &prop, AsyncWebSocketClient &client,
                    int status, const char *msg) {
    prop["error"] = msg;
    prop["status"] = status;
    String jsonStr;
    serializeJson(prop, jsonStr);
    client.text(jsonStr.c_str(), jsonStr.length());
  }

  void handleWS(AsyncWebSocket *server, AsyncWebSocketClient *client,
                AwsEventType type, void *arg, const uint8_t *rawData,
                size_t len, ThingDevice *device) {
    if (type == WS_EVT_DISCONNECT || type == WS_EVT_ERROR) {
      device->removeEventSubscriptions(client->id());
      return;
    }

    // Ignore all others except data packets
    if (type != WS_EVT_DATA)
      return;

    // Only consider non fragmented data
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (!info->final || info->index != 0 || info->len != len)
      return;

    // Web Thing only specifies text, not binary websocket transfers
    if (info->opcode != WS_TEXT)
      return;

    // In theory we could just have one websocket for all Things and react on
    // the server->url() to route data. Controllers will however establish a
    // separate websocket connection for each Thing anyway as of in the spec.
    // For now each Thing stores its own Websocket connection object therefore.

    // Parse request
    JsonDocument newProp;
    auto error = deserializeJson(newProp, rawData, len);
    if (error) {
      sendErrorMsg(newProp, *client, 400, "Invalid json");
      return;
    }

    String messageType = newProp["messageType"].as<String>();
    JsonVariant dataVariant = newProp["data"];
    if (!dataVariant.is<JsonObject>()) {
      sendErrorMsg(newProp, *client, 400, "data must be an object");
      return;
    }

    JsonObject data = dataVariant.as<JsonObject>();

    if (messageType == "setProperty") {
      for (JsonPair kv : data) {
        device->setProperty(kv.key().c_str(), kv.value());
      }
    } else if (messageType == "requestAction") {
      for (JsonPair kv : data) {
        JsonDocument *actionRequest =
            new JsonDocument;

        JsonObject actionObj = actionRequest->to<JsonObject>();
        JsonObject nested = actionObj[kv.key()].to<JsonObject>();

        for (JsonPair kvInner : kv.value().as<JsonObject>()) {
          nested[kvInner.key()] = kvInner.value();
        }

        ThingActionObject *obj = device->requestAction(actionRequest);
        if (obj != nullptr) {
          obj->setNotifyFunction(std::bind(&ThingDevice::sendActionStatus,
                                           device, std::placeholders::_1));
          device->sendActionStatus(obj);

          obj->start();
        } else {
          // requestAction() only takes ownership of actionRequest on
          // success; free it ourselves on failure to avoid a leak.
          delete actionRequest;
        }
      }
    } else if (messageType == "addEventSubscription") {
      for (JsonPair kv : data) {
        ThingEvent *event = device->findEvent(kv.key().c_str());
        if (event) {
          device->addEventSubscription(client->id(), event->id);
        }
      }
    }
  }

  void sendChangedProperties(ThingDevice *device) {
    // Prepare one buffer per device
    JsonDocument message;
    message["messageType"] = "propertyStatus";
    JsonObject prop = message["data"].to<JsonObject>();
    bool dataToSend = false;
    ThingItem *item = device->firstProperty;
    while (item != nullptr) {
      ThingDataValue *value = item->changedValueOrNull();
      if (value) {
        dataToSend = true;
        item->serializeValue(prop);
      }
      item = item->next;
    }
    if (dataToSend) {
      String jsonStr;
      serializeJson(message, jsonStr);
      // Inform all connected ws clients of a Thing about changed properties
      ((AsyncWebSocket *)device->ws)->textAll(jsonStr);
    }
  }
#endif

  void handleUnknown(AsyncWebServerRequest *request) {
    if (!verifyHost(request)) {
      return;
    }
    request->send(404);
  }

  void handleOptions(AsyncWebServerRequest *request) {
    if (!verifyHost(request)) {
      return;
    }
    request->send(204);
  }

  void handleThings(AsyncWebServerRequest *request) {
    if (!verifyHost(request)) {
      return;
    }
    AsyncResponseStream *response =
        request->beginResponseStream("application/json");

    JsonDocument buf;
    JsonArray things = buf.to<JsonArray>();
    ThingDevice *device = this->firstDevice;
    while (device != nullptr) {
      JsonObject descr = things.add<JsonObject>();
      device->serialize(descr, ip, port);
      descr["href"] = "/things/" + device->id;
      device = device->next;
    }

    serializeJson(things, *response);
    request->send(response);
  }

  void handleThing(AsyncWebServerRequest *request, ThingDevice *&device) {
    if (!verifyHost(request)) {
      return;
    }
    AsyncResponseStream *response =
        request->beginResponseStream("application/json");

    JsonDocument buf;
    JsonObject descr = buf.to<JsonObject>();
    device->serialize(descr, ip, port);

    serializeJson(descr, *response);
    request->send(response);
  }

  void handleThingPropertyGet(AsyncWebServerRequest *request,
                              ThingItem *item) {
    if (!verifyHost(request)) {
      return;
    }
    AsyncResponseStream *response =
        request->beginResponseStream("application/json");

    JsonDocument doc;
    JsonObject prop = doc.to<JsonObject>();
    item->serializeValue(prop);
    serializeJson(prop, *response);
    request->send(response);
  }

  void handleThingActionGet(AsyncWebServerRequest *request,
                            ThingDevice *device, ThingAction *action) {
    if (!verifyHost(request)) {
      return;
    }

    String url = request->url();
    String base = "/things/" + device->id + "/actions/" + action->id;
    if (url == base || url == base + "/") {
      AsyncResponseStream *response =
          request->beginResponseStream("application/json");
      JsonDocument doc;
      JsonArray queue = doc.to<JsonArray>();
      device->serializeActionQueue(queue, action->id);
      serializeJson(queue, *response);
      request->send(response);
    } else {
      String actionId = url.substring(base.length() + 1);
      const char *actionIdC = actionId.c_str();
      const char *slash = strchr(actionIdC, '/');

      if (slash) {
        actionId = actionId.substring(0, slash - actionIdC);
      }

      ThingActionObject *obj = device->findActionObject(actionId.c_str());
      if (obj == nullptr) {
        request->send(404);
        return;
      }

      AsyncResponseStream *response =
          request->beginResponseStream("application/json");
      JsonDocument doc;
      JsonObject o = doc.to<JsonObject>();
      obj->serialize(o, device->id);
      serializeJson(o, *response);
      request->send(response);
    }
  }

  void handleThingActionDelete(AsyncWebServerRequest *request,
                               ThingDevice *device, ThingAction *action) {
    if (!verifyHost(request)) {
      return;
    }

    String url = request->url();
    String base = "/things/" + device->id + "/actions/" + action->id;
    if (url == base || url == base + "/") {
      request->send(404);
      return;
    }

    String actionId = url.substring(base.length() + 1);
    const char *actionIdC = actionId.c_str();
    const char *slash = strchr(actionIdC, '/');

    if (slash) {
      actionId = actionId.substring(0, slash - actionIdC);
    }

    device->removeAction(actionId);
    request->send(204);
  }

  void handleThingActionPost(AsyncWebServerRequest *request,
                             ThingDevice *device, ThingAction *action) {
    if (!verifyHost(request)) {
      return;
    }

    BodyData *bodyData = getBodyData(request);
    if (bodyData == nullptr || !bodyData->complete) {
      request->send(422); // unprocessable entity (b/c no body)
      freeBodyData(request);
      return;
    }

    JsonDocument *newBuffer = new JsonDocument;
    auto error = deserializeJson(*newBuffer, (const char *)bodyData->data);
    freeBodyData(request);
    if (error) { // unable to parse json
      request->send(500);
      delete newBuffer;
      return;
    }

    JsonObject newAction = newBuffer->as<JsonObject>();

    // A missing key deserializes to a null JsonVariant, so check for
    // that rather than checking the value's type (an action whose
    // parameter isn't an int would otherwise be wrongly rejected here).
    if (newAction[action->id].isNull()) {
      request->send(400);
      delete newBuffer;
      return;
    }

    ThingActionObject *obj = device->requestAction(newBuffer);

    if (obj == nullptr) {
      request->send(500);
      delete newBuffer;
      return;
    }

#ifndef WITHOUT_WS
    obj->setNotifyFunction(std::bind(&ThingDevice::sendActionStatus, device,
                                     std::placeholders::_1));
#endif

    JsonDocument respBuffer;
    JsonObject item = respBuffer.to<JsonObject>();
    obj->serialize(item, device->id);
    String jsonStr;
    serializeJson(item, jsonStr);
    AsyncWebServerResponse *response =
        request->beginResponse(201, "application/json", jsonStr);
    request->send(response);

    obj->start();
  }

  void handleThingEventGet(AsyncWebServerRequest *request, ThingDevice *device,
                           ThingItem *item) {
    if (!verifyHost(request)) {
      return;
    }
    AsyncResponseStream *response =
        request->beginResponseStream("application/json");

    JsonDocument doc;
    JsonArray queue = doc.to<JsonArray>();
    device->serializeEventQueue(queue, item->id);
    serializeJson(queue, *response);
    request->send(response);
  }

  void handleThingPropertiesGet(AsyncWebServerRequest *request,
                                ThingItem *rootItem) {
    if (!verifyHost(request)) {
      return;
    }
    AsyncResponseStream *response =
        request->beginResponseStream("application/json");

    JsonDocument doc;
    JsonObject prop = doc.to<JsonObject>();
    ThingItem *item = rootItem;
    while (item != nullptr) {
      item->serializeValue(prop);
      item = item->next;
    }
    serializeJson(prop, *response);
    request->send(response);
  }

  void handleThingActionsGet(AsyncWebServerRequest *request,
                             ThingDevice *device) {
    if (!verifyHost(request)) {
      return;
    }
    AsyncResponseStream *response =
        request->beginResponseStream("application/json");

    JsonDocument doc;
    JsonArray queue = doc.to<JsonArray>();
    device->serializeActionQueue(queue);
    serializeJson(queue, *response);
    request->send(response);
  }

  void handleThingActionsPost(AsyncWebServerRequest *request,
                              ThingDevice *device) {
    if (!verifyHost(request)) {
      return;
    }

    BodyData *bodyData = getBodyData(request);
    if (bodyData == nullptr || !bodyData->complete) {
      request->send(422); // unprocessable entity (b/c no body)
      freeBodyData(request);
      return;
    }

    JsonDocument *newBuffer = new JsonDocument;
    auto error = deserializeJson(*newBuffer, (const char *)bodyData->data);
    freeBodyData(request);
    if (error) { // unable to parse json
      request->send(500);
      delete newBuffer;
      return;
    }

    JsonObject newAction = newBuffer->as<JsonObject>();

    if (newAction.size() != 1) {
      request->send(400);
      delete newBuffer;
      return;
    }

    ThingActionObject *obj = device->requestAction(newBuffer);

    if (obj == nullptr) {
      request->send(500);
      delete newBuffer;
      return;
    }

#ifndef WITHOUT_WS
    obj->setNotifyFunction(std::bind(&ThingDevice::sendActionStatus, device,
                                     std::placeholders::_1));
#endif

    JsonDocument respBuffer;
    JsonObject item = respBuffer.to<JsonObject>();
    obj->serialize(item, device->id);
    String jsonStr;
    serializeJson(item, jsonStr);
    AsyncWebServerResponse *response =
        request->beginResponse(201, "application/json", jsonStr);
    request->send(response);

    obj->start();
  }

  void handleThingEventsGet(AsyncWebServerRequest *request,
                            ThingDevice *device) {
    if (!verifyHost(request)) {
      return;
    }
    AsyncResponseStream *response =
        request->beginResponseStream("application/json");

    JsonDocument doc;
    JsonArray queue = doc.to<JsonArray>();
    device->serializeEventQueue(queue);
    serializeJson(queue, *response);
    request->send(response);
  }

  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len,
                  size_t index, size_t total) {
    // Leave room for a trailing null terminator.
    if (total >= ESP_MAX_PUT_BODY_SIZE - 1) {
      if (index == 0) {
        // Only reply once per request; report clearly instead of
        // silently dropping the body and later returning a
        // misleading "no body" (422) response.
        request->send(413); // payload too large
      }
      return;
    }

    BodyData *bodyData = getBodyData(request);
    if (bodyData == nullptr) {
      bodyData = (BodyData *)malloc(sizeof(BodyData));
      memset(bodyData, 0, sizeof(BodyData));
      request->_tempObject = bodyData;
    }

    memcpy(&bodyData->data[index], data, len);
    bodyData->len = index + len;
    if (index + len == total) {
      bodyData->data[bodyData->len] = '\0';
      bodyData->complete = true;
    }
  }

  void handleThingPropertyPut(AsyncWebServerRequest *request,
                              ThingDevice *device, ThingProperty *property) {
    if (!verifyHost(request)) {
      return;
    }

    BodyData *bodyData = getBodyData(request);
    if (bodyData == nullptr || !bodyData->complete) {
      request->send(422); // unprocessable entity (b/c no body)
      freeBodyData(request);
      return;
    }

    JsonDocument newBuffer;
    auto error = deserializeJson(newBuffer, bodyData->data);
    freeBodyData(request);
    if (error) { // unable to parse json
      request->send(500);
      return;
    }
    JsonObject newProp = newBuffer.as<JsonObject>();

    // A missing key deserializes to a null JsonVariant, so check for
    // that rather than checking the value's type (a property whose
    // value isn't an int would otherwise be wrongly rejected here).
    if (newProp[property->id].isNull()) {
      request->send(400);
      return;
    }

    device->setProperty(property->id.c_str(), newProp[property->id]);

    AsyncResponseStream *response =
        request->beginResponseStream("application/json");
    serializeJson(newProp, *response);
    request->send(response);
  }
};