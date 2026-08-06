#include <cassert>
#include <cmath>
#include <iostream>

#include "../control/lib/production_control/src/competition_actuators.h"
#include "../control/lib/production_control/src/production_control.h"

using namespace production_control;

SensorInput nominal(uint64_t now=1'000'000) {
  SensorInput input{};
  input.safety=AuthoritativeSafety::Running;
  input.nowUs=now;
  input.heartbeat=input.imuValid=input.tofValid=input.gnssValid=true;
  input.powerValid=input.vescValid=true;
  input.heartbeatUs=input.imuUs=input.tofUs=input.gnssUs=now;
  input.powerUs=input.vescUs=now;
  input.tofM=.45f;
  input.busVoltageV=12.0f;
  input.currentA=3.0f;
  input.vescErpm=1000.0f;
  input.groundSpeedMps=1.0f;
  return input;
}

void testAutoWaypoint() {
  Config config{};
  Controller controller(config);
  Waypoint route[]={{20.0f,0.0f},{30.0f,5.0f}};
  assert(controller.setWaypoints(route,2,1,AuthoritativeSafety::Disarmed).ack==Ack::Accepted);
  assert(controller.setMode(ControlMode::AutoWaypoint,2,AuthoritativeSafety::Disarmed).ack==Ack::Accepted);
  auto input=nominal();
  const auto output=controller.step(input);
  assert(output.safetyRequest==SafetyRequest::None);
  assert(output.physicalGate);
  assert(output.propulsion>0);
  assert(output.waypointDistanceM>19.9f);
}

void testTofGracefulDegradation() {
  Controller controller;
  controller.setManual({0,0,0,.4f},100);
  controller.setMode(ControlMode::AttitudeAssist,1,AuthoritativeSafety::Disarmed);
  controller.setManual({0,0,0,.4f},1'000'000);
  auto input=nominal();
  input.tofValid=false;
  const auto output=controller.step(input);
  assert(output.safetyRequest==SafetyRequest::None);
  assert(output.flags&HeightDegraded);
}

void testPowerProtection() {
  Controller controller;
  controller.setManual({0,0,0,.8f},1'000'000);
  auto input=nominal();
  input.busVoltageV=9.0f;
  auto output=controller.step(input);
  assert(output.safetyRequest==SafetyRequest::None);
  assert(output.flags&VoltageLimited);
  assert(output.propulsion<=.04f);

  input=nominal(2'000'000);
  input.busVoltageV=8.0f;
  controller.setManual({0,0,0,.8f},input.nowUs);
  output=controller.step(input);
  assert(output.safetyRequest==SafetyRequest::Fault);
  assert(output.reason==StopReason::LowVoltage);
}

void testStallAndPitchProtection() {
  Controller controller;
  controller.setManual({0,0,0,.8f},1'000'000);
  auto input=nominal();
  input.currentA=10.0f;
  input.vescErpm=0;
  auto output=controller.step(input);
  assert(output.safetyRequest==SafetyRequest::None);
  input.nowUs=2'100'001;
  input.heartbeatUs=input.powerUs=input.vescUs=input.imuUs=input.tofUs=input.gnssUs=input.nowUs;
  controller.setManual({0,0,0,.8f},input.nowUs);
  output=controller.step(input);
  assert(output.safetyRequest==SafetyRequest::Fault);
  assert(output.reason==StopReason::MotorStall);

  Controller attitude;
  attitude.setMode(ControlMode::AttitudeAssist,1,AuthoritativeSafety::Disarmed);
  attitude.setManual({0,0,0,.8f},1'000'000);
  input=nominal();
  input.pitchRad=.40f;
  output=attitude.step(input);
  assert(output.flags&PitchPriority);
  assert(output.throttleLimit<=.25f);
}

void testActuatorMapping() {
  ServoMapper mapper(ServoTuning(1200,1500,1800,300,false));
  auto result=mapper.map(1.0f,1.0f);
  assert(result.pulseUs==1530);  // dt is intentionally capped at 0.1 s.
  DutyRamp ramp(.6f,2.0f,.35f);
  ramp.setTarget(.6f);
  const float first=ramp.step(.02f);
  assert(first>0&&first<.02f);
  ramp.stopImmediate();
  assert(ramp.applied()==0);
  assert(!motorRelayRequired(0.0f));
  assert(motorRelayRequired(first));
  assert(motorRelayRequired(-first));
  assert(!motorRelayRequired(NAN));
}

int main() {
  testAutoWaypoint();
  testTofGracefulDegradation();
  testPowerProtection();
  testStallAndPitchProtection();
  testActuatorMapping();
  std::cout << "controller tests passed\n";
}
