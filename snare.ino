#include <V2Base.h>
#include <V2Buttons.h>
#include <V2Device.h>
#include <V2LED.h>
#include <V2Link.h>
#include <V2MIDI.h>
#include <V2Music.h>
#include <V2PowerSupply.h>
#include <V2Solenoids.h>
#include <V2Stepper.h>

namespace {
  namespace LEDs {
    enum Position {
      Button,
      Pulse = Button + 2,
      Step  = Pulse + 2,
      size,
    };
  }

  V2Device::Info            Info{V2DeviceInfo("com.versioduo.snare", 1, "versioduo:samd:drum")};
  V2LED::WS2812<LEDs::size> LED(PIN_LED_WS2812, sercom2, SPI_PAD_0_SCK_1, PIO_SERCOM);
  V2LED::WS2812<169>        LEDExt(PIN_LED_WS2812_EXT, sercom4, SPI_PAD_0_SCK_1, PIO_SERCOM);
  V2Link::Port              Plug(&SerialPlug, PIN_SERIAL_PLUG_TX_ENABLE, "plug");
  V2Link::Port              Socket(&SerialSocket, PIN_SERIAL_SOCKET_TX_ENABLE, "socket");
  V2Base::Timer::Periodic   Timer(2, 200000);

  // Try to spread the power switching noise; run the timers with slightly
  // different periods, so they don't all start the rising edge of the PWM
  // period at the same time.
  std::array PWM{
    V2Base::Timer::PWM(0, 20000),
    V2Base::Timer::PWM(1, 20100),
    V2Base::Timer::PWM(2, 20200),
    V2Base::Timer::PWM(3, 20300),
    V2Base::Timer::PWM(4, 20400),
  };

  std::array ADC{
    V2Base::Analog::ADC(0),
    V2Base::Analog::ADC(1),
  };

  class Power : public V2PowerSupply {
  public:
    Power() : V2PowerSupply({.min{12}, .max{30}}) {}

    auto begin() {
      pinMode(PIN_PULSE_ENABLE, OUTPUT);
      pulse.off();
      pinMode(PIN_STEP_ENABLE, OUTPUT);
      step.off();
    }

    struct {
      auto on() -> void {
        digitalWrite(PIN_PULSE_ENABLE, HIGH);
      }

      auto off() -> void {
        digitalWrite(PIN_PULSE_ENABLE, LOW);
      }
    } pulse;

    struct {
      auto on() -> void {
        digitalWrite(PIN_STEP_ENABLE, LOW);
      }

      auto off() -> void {
        digitalWrite(PIN_STEP_ENABLE, HIGH);
      }
    } step;

  private:
    auto handleMeasurement() -> float override {
      auto id{V2Base::Analog::ADC::getID(PIN_VOLTAGE_SENSE)};
      auto channel{V2Base::Analog::ADC::getChannel(PIN_VOLTAGE_SENSE)};
      return 36.f * ADC[id].readChannel(channel);
    }

    // The pulse part handles the switching itself to measure the solenoid resistance.
    auto handleOn() -> void override {
      step.on();
    }

    auto handleOff() -> void override {
      pulse.off();
      step.off();
    }

    auto handleNotify(float voltage) -> void override {
      // Power interruption, or commands without a power connection show yellow LEDs.
      if (voltage < config.min) {
        LED.flash({V2Colour::Yellow, 1, 0.25}, 0.5);
        return;
      }

      // Over-voltage shows red LEDs.
      if (voltage > config.max) {
        LED.flash({V2Colour::Red, 1, 1}, 0.5);
        return;
      }

      // The number of green LEDs shows the voltage.
      auto fraction{voltage / float(config.max)};
      auto n{ceil(float(LEDs::size - LEDs::Pulse) * fraction)};
      LED.flash({V2Colour::Green, 1, 0.25}, 0.5, LEDs::Pulse, n);
    }
  } Power;

  class Pulse : public V2Solenoids<2> {
  public:
    constexpr Pulse() :
      V2Solenoids({
        .current{.max{1}, .alpha{0.001}},
        .resistance{.min{10}, .max{60}},
        .fade{.inSec{0.35}, .outSec{0.35}},
        .hold{.peakUsec{100 * 1000}, .fraction{0.5}},
      }) {}

    auto setPower(PowerState state) -> bool override {
      switch (state) {
        case PowerState::On:
          if (!Power.on())
            return false;
          Power.pulse.on();
          break;

        case PowerState::Off:
          Power.pulse.off();
          break;
      }

      return true;
    }

    auto readVoltage() -> float override {
      return Power.getVoltage();
    }

    auto readCurrent() -> float override {
      auto id{V2Base::Analog::ADC::getID(PIN_PULSE_CURRENT_SENSE)};
      auto channel{V2Base::Analog::ADC::getChannel(PIN_PULSE_CURRENT_SENSE)};
      auto voltage{3.3f * ADC[id].readChannel(channel)};

      // Current sense ratio 3900, 1350 Ω sense resistor, 9.5 A maximum range.
      return voltage / 1350.f * 3900.f;
    }

    // One single port at a time is connected to 3.3V. The load and a 100Ω resistor
    // form a voltage divider.
    auto readResistanceVoltage() -> float override {
      auto id{V2Base::Analog::ADC::getID(PIN_RESISTANCE_SENSE)};
      auto channel{V2Base::Analog::ADC::getChannel(PIN_RESISTANCE_SENSE)};
      return 3.3f * ADC[id].readChannel(channel);
    }

    auto setLED(LEDMode state, uint8_t port = 0, float value = -1) -> void override {
      if (LED.rainbow())
        return;

      switch (state) {
        case LEDMode::Off:
          LED.brightness(0, LEDs::Pulse + port);
          break;

        case LEDMode::Initialize:
          LED.hsv({V2Colour::Cyan, 1, 0.25}, LEDs::Button + 0, 2);
          break;

        case LEDMode::Ready:
          LED.hsv({V2Colour::Orange, 1, 0.25}, LEDs::Button + 0, 2);
          break;

        case LEDMode::Resistance:
          // Map the fraction of the configured resistance range from cyan to magenta.
          LED.hsv({V2Colour::Cyan + (120.f * value), 1, 0.15}, LEDs::Pulse + port);
          break;

        case LEDMode::Power: {
          auto fraction{powf(value / 100.f, 8)};
          LED.hsv({V2Colour::Orange, 1, 0.3f + (0.3f * fraction)}, LEDs::Pulse + port);
        } break;

        case LEDMode::ShortCircuit:
          LED.hsv({V2Colour::Red, 1, 1}, LEDs::Pulse + port);
          break;

        case LEDMode::OverCurrent:
          LED.flash({V2Colour::Magenta, 1, 1}, 0.2);
          break;
      }
    }

    auto setPWMDuty(uint8_t port, float duty) -> void override {
      auto id{V2Base::Timer::PWM::getID(PIN_PULSE_CHANNEL + port)};
      PWM[id].setDuty(PIN_PULSE_CHANNEL + port, duty);
    }
  } Pulse;

  class Step : public V2Stepper::Motor {
  public:
    Step() :
      Motor(
        {
          .ampere{0.5},
          .microstepsShift{3},
          .home{.speed{200}, .stall{0.04}},
          .speed{.min{100}, .max{4000}, .accel{16000}},
        },
        &Timer,
        &SPI,
        PIN_STEP_SELECT,
        PIN_STEP_STEP) {}

    auto queue(float position, float speed = 1) {
      if (position < 0) {
        _queue.position = -1;
        return;
      }

      if (!isBusy()) {
        setPosition(position, speed);
        return;
      }

      // Queue position to move to after the current task has finished.
      _queue.position = position;
      _queue.speed    = speed;
    }

  private:
    struct {
      float position{-1};
      float speed{};
    } _queue;

    auto handleMovement(Move move) -> void override {
      switch (move) {
        case Move::Forward:
          LED.hsv({V2Colour::Cyan, 1, 0.25}, LEDs::Step);
          break;

        case Move::Reverse:
          LED.hsv({V2Colour::Orange, 1, 0.25}, LEDs::Step);
          break;

        case Move::Stop:
          if (_queue.position < 0) {
            LED.hsv({V2Colour::Green, 1, 0.15}, LEDs::Step);
            break;
          }

          setPosition(_queue.position, _queue.speed);
          _queue.position = -1;
          break;
      }
    }
  } Step;

  class {
  public:
    auto play(float v) {
      if (_rainbow > 0.f)
        return;

      if (v > 0.f) {
        // Dim to warm.
        auto saturation{0.8f + (0.2f * (1.f - v))};
        auto brightness{0.3f + (0.7f * v * _brightness)};
        LEDExt.hsv({V2Colour::Orange, saturation, brightness});
      } else {
        LEDExt.brightness(0);
      }
    }

    auto brightness() -> float {
      return _brightness;
    }

    auto brightness(float v) {
      _brightness = v;

      if (_rainbow > 0.f)
        LEDExt.rainbow(1, 8.f - (_rainbow * 5.f), 0.1f + (0.9f * _brightness));
    }

    auto rainbow() -> float {
      return _rainbow;
    }

    auto rainbow(float v) {
      _rainbow = v;
      if (_rainbow > 0.f)
        LEDExt.rainbow(1, 8.f - (_rainbow * 5.f), 0.1f + (0.9f * _brightness));
      else
        LEDExt.reset();
    }

    auto reset() {
      LEDExt.reset();
      _brightness = 100.f / 127.f;
      _rainbow    = 0;
    }

  private:
    float _brightness{100.f / 127.f};
    float _rainbow{};
  } Light;

  class {
  public:
    auto position() -> float {
      return _position;
    }

    auto position(float position) -> void {
      _position = position;
      Step.setPosition(_position * 25.f);
    }

    auto reset() -> void {
      _position = 0;
    }

    auto home() -> void {
      // Move past the detected home position for an increased pressure when positioning to 0.
      static constexpr auto pressure{[] {
        Step.initializePosition(8);
        Step.setPosition(0, 0.01);
      }};

      static constexpr auto home{[] { Step.home(200, 0, pressure); }};

      // Move a few steps before calling home(). We do not move any steps back after the stall detection in home();
      // from this position we cannot reliably detect a stall again.
      Step.setPosition(20, 0.01, home);
      Step.hold(0.2);
    }

  private:
    float _position{};
  } Snare;

  class Device : public V2Device {
  public:
    Device() : V2Device() {
      metadata.vendor      = "Versio Duo";
      metadata.product     = "V2 snare";
      metadata.description = "Stiff wires held under tension against the lower skin";
      metadata.home        = "https://versioduo.com/#snare";
      system.download      = "https://versioduo.com/download";
      system.configure     = "https://versioduo.com/configure";
      usb.pid              = 0xe9f0; // https://github.com/versioduo/arduino-board-package/blob/main/boards.txt
      usb.ports.standard   = 8;
    }

    auto allNotesOff() {
      _timeoutUsec = V2Base::getUsec();
      Light.reset();

      if (!Power.on())
        return;

      if (!_ready || _force.trigger()) {
        Snare.home();
        _ready = true;
      }
    }

  private:
    enum class CC {
      Volume  = V2MIDI::CC::ChannelVolume,
      Snare   = V2MIDI::CC::ModulationWheel,
      Light   = V2MIDI::CC::Controller89,
      Rainbow = V2MIDI::CC::Controller90,
    };

    uint8_t             _volume{100};
    uint32_t            _timeoutUsec{};
    bool                _ready{};
    V2Music::ForcedStop _force;

    auto handleReset() -> void override {
      _volume      = 100;
      _timeoutUsec = 0;
      _ready       = false;
      _force.reset();
      Light.reset();
      Pulse.reset();
      Snare.reset();
      Step.reset();
      Power.off();
      LED.reset();
    }

    auto handleLoop() -> void override {
      if (_timeoutUsec == 0)
        return;

      if (V2Base::getUsecSince(_timeoutUsec) < 600 * 1000 * 1000)
        return;

      reset();
    }

    auto power() -> bool {
      bool continuous{};
      if (!Power.on(continuous))
        return false;

      if (!continuous) {
        Step.reset();
        _ready = false;
      }

      if (!_ready)
        allNotesOff();

      return true;
    }

    auto scaleVolume(uint8_t velocity) -> float {
      switch (_volume) {
        case 0:
          return 0.f;

        case 1 ... 99: {
          auto fraction{float(_volume) / 100.f};
          return (float(velocity) / 127.f) * fraction;
        }

        case 100:
          return float(velocity) / 127.f;

        case 101 ... 127: {
          auto fraction{float(_volume - 100) / 27.f};
          return powf(float(velocity) / 127.f, 1.f - (0.5f * fraction));
        }

        default:
          abort();
      }
    }

    auto trigger(uint8_t port, uint8_t velocity) {
      static constexpr struct {
        struct {
          float watts{0.6};
          float seconds{0.035};
        } min;
        struct {
          float watts{8};
          float seconds{0.015};
        } max;
      } range;

      if (velocity == 0)
        return;

      auto fraction{powf(scaleVolume(velocity), 1.5)};
      auto watts{range.min.watts};
      watts += (range.max.watts - range.min.watts) * powf(fraction, 2);
      auto seconds{range.min.seconds};
      seconds += (range.max.seconds - range.min.seconds) * powf(fraction, 0.5);
      Pulse.triggerPort(port, watts, seconds);
    }

    auto play(uint8_t channel, uint8_t note, uint8_t velocity) {
      if (!power())
        return;

      _timeoutUsec = V2Base::getUsec();

      switch (note) {
        case V2MIDI::C(3):
          Light.play(float(velocity) / 127.f);
          trigger(0, velocity);
          break;

        case V2MIDI::Cs(3):
          Light.play(float(velocity) / 127.f);
          trigger(1, velocity);
          break;
      }
    }

    auto handleNote(uint8_t channel, uint8_t note, uint8_t velocity) -> void override {
      play(channel, note, velocity);
    }

    auto handleNoteOff(uint8_t channel, uint8_t note, uint8_t velocity) -> void override {
      play(channel, note, 0);
    }

    auto handleControlChange(uint8_t channel, uint8_t controller, uint8_t value) -> void override {
      if (channel != 0)
        return;

      if (!power())
        return;

      _timeoutUsec = V2Base::getUsec();

      switch (controller) {
        case uint8_t(CC::Volume):
          _volume = value;
          break;

        case uint8_t(CC::Snare):
          Snare.position(float(value) / 127.f);
          break;

        case uint8_t(CC::Light):
          Light.brightness(float(value) / 127.f);
          break;

        case uint8_t(CC::Rainbow):
          Light.rainbow(float(value) / 127.f);
          break;

        case V2MIDI::CC::AllSoundOff:
        case V2MIDI::CC::AllNotesOff:
          allNotesOff();
          break;
      }
    }

    auto handleSystemReset() -> void override {
      reset();
    }

    auto exportInput(JsonObject json) -> void override {
      JsonArray jsonControllers{json["controllers"].to<JsonArray>()};
      {
        JsonObject jsonController{jsonControllers.add<JsonObject>()};
        jsonController["name"]   = "Volume";
        jsonController["number"] = uint8_t(CC::Volume);
        jsonController["value"]  = _volume;
      }
      {
        JsonObject jsonController{jsonControllers.add<JsonObject>()};
        jsonController["name"]   = "Snare";
        jsonController["number"] = uint8_t(CC::Snare);
        jsonController["value"]  = uint8_t(Snare.position() * 127.f);
      }
      {
        JsonObject jsonController{jsonControllers.add<JsonObject>()};
        jsonController["name"]   = "Light";
        jsonController["number"] = uint8_t(CC::Light);
        jsonController["value"]  = uint8_t(Light.brightness() * 127.f);
      }
      {
        JsonObject jsonController{jsonControllers.add<JsonObject>()};
        jsonController["name"]   = "Rainbow";
        jsonController["number"] = uint8_t(CC::Rainbow);
        jsonController["value"]  = uint8_t(Light.rainbow() * 127.f);
      }

      JsonArray jsonNotes{json["notes"].to<JsonArray>()};
      {
        JsonObject jsonNote{jsonNotes.add<JsonObject>()};
        jsonNote["name"]   = "Trigger 1";
        jsonNote["number"] = V2MIDI::C(3);
      }
      {
        JsonObject jsonNote{jsonNotes.add<JsonObject>()};
        jsonNote["name"]   = "Trigger 2";
        jsonNote["number"] = V2MIDI::Cs(3);
      }
    }

    auto exportSystem(JsonObject json) -> void override {
      JsonObject jsonPower{json["power"].to<JsonObject>()};
      jsonPower["voltage"]       = serialized(String(Power.getVoltage(), 1));
      jsonPower["interruptions"] = Power.getInterruptions();

      if (Power.isOn())
        jsonPower["current"] = serialized(String(Pulse.getCurrent(), 1));

      JsonArray jsonOutputs{json["solenoids"].to<JsonArray>()};
      {
        JsonObject output{jsonOutputs.add<JsonObject>()};
        output["resistance"] = serialized(String(Pulse.getResistance(0), 1));
      }
      {
        JsonObject output{jsonOutputs.add<JsonObject>()};
        output["resistance"] = serialized(String(Pulse.getResistance(1), 1));
      }
    }
  } Device;

  // Dispatch MIDI packets.
  class MIDI {
  public:
    auto loop() {
      if (!Device.usb.midi.receive(_midi))
        return;

      if (_midi.port == 0) {
        Device.dispatch(&Device.usb.midi, &_midi);

      } else {
        V2Link::Packet p(_midi.port - 1, _midi);
        p.midi.port = 0;
        Socket.send(p);
      }
    }

  private:
    V2MIDI::Packet _midi{};
  } MIDI;

  // Dispatch Link packets.
  class Link : public V2Link {
  public:
    Link() : V2Link(&Plug, &Socket) {
      Device.link = this;
    }

  private:
    // Receive a host event from our parent device.
    auto receivePlug(V2Link::Packet& p) -> void override {
      if (p.type == V2Link::Packet::Type::MIDI)
        Device.dispatch(&Plug, &p.midi);
    }

    // Forward children device events to the host.
    auto receiveSocket(V2Link::Packet& p) -> void override {
      if (p.type == V2Link::Packet::Type::MIDI) {
        p.midi.port = p.address;
        Device.usb.midi.send(p.midi);
      }
    }
  } Link;

  class Button : public V2Buttons::Button {
  public:
    Button() : V2Buttons::Button(&_config, PIN_BUTTON) {}

  private:
    const V2Buttons::Config _config{.clickUsec{200 * 1000}, .holdUsec{500 * 1000}};

    auto handleClick(uint8_t count) -> void override {
      switch (count) {
        case 0:
          Device.allNotesOff();
          break;

        case 1:
          Device.reset();
          break;
      }
    }

    auto handleHold(uint8_t count) -> void override {
      LEDExt.rainbow(1, 2, 0.8);
    }

    auto handleRelease() -> void override {
      LEDExt.reset();
    }
  } Button;
}

auto setup() -> void {
  Serial.begin(9600);
  SPI.begin();

  LED.begin();
  LED.brightnessMax(0.5);
  LEDExt.begin();
  LEDExt.brightnessMax(0.75);

  // Set the SERCOM interrupt priority, it requires a stable ~300 kHz interrupt
  // frequency. The call needs to be after begin().
  Link.begin();
  setSerialPriority(&SerialPlug, 2);
  setSerialPriority(&SerialSocket, 1);

  Power.begin();

  for (auto& p : PWM)
    p.begin();

  for (uint8_t p{PIN_PULSE_CHANNEL}; p < PIN_PULSE_CHANNEL + PIN_PULSE_CHANNEL_N; p++)
    V2Base::Timer::PWM::setupPin(p);

  for (auto& a : ADC)
    a.begin();

  for (uint8_t p : {PIN_RESISTANCE_SENSE, PIN_VOLTAGE_SENSE, PIN_PULSE_CURRENT_SENSE})
    ADC[V2Base::Analog::ADC::getID(p)].addChannel(V2Base::Analog::ADC::getChannel(p));

  Step.begin();

  // The priority needs to be lower than the SERCOM priorities.
  Timer.begin([] { Step.tick(); });
  Timer.setPriority(3);

  Device.begin();
  Button.begin();
  Device.reset();
}

auto loop() -> void {
  LED.loop();
  LEDExt.loop();
  MIDI.loop();
  Link.loop();
  V2Buttons::loop();
  Power.loop();
  Pulse.loop();
  Step.loop();
  Device.loop();

  if (Link.idle() && Device.idle())
    Device.sleep();
}
