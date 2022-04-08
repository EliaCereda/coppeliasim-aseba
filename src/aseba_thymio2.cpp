/*
Aseba - an event-based framework for distributed robot control
Copyright (C) 2007--2013:
  Stephane Magnenat <stephane at magnenat dot net>
  (http://stephane.magnenat.net)
  and other contributors, see authors.txt for details

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published
by the Free Software Foundation, version 3 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include <algorithm>
#include "aseba_thymio2.h"
#include "aseba_thymio2_natives.h"
#include "common/productids.h"
#include "common/utils/utils.h"

AsebaThymio2::AsebaThymio2(int node_id, std::string _name,
                           std::array<uint8_t, 16> uuid_, std::string friendly_name_):
  CoppeliaSimAsebaNode(node_id, _name, uuid_, friendly_name_),
  // SingleVMNodeGlue(std::move(robotName), nodeId),
  // sdCardFileNumber(-1),
  timer0(std::bind(&AsebaThymio2::timer0Timeout, this), 0),
  timer1(std::bind(&AsebaThymio2::timer1Timeout, this), 0),
  timer100Hz(std::bind(&AsebaThymio2::timer100HzTimeout, this), 0.01),
  counter100Hz(0), oldTimerPeriod{0, 0}, oldMicThreshold(0), first(true) {
  thymio_variables = reinterpret_cast<thymio_variables_t *>(&variables);

  // this simulated Thymio complies with firmware 11 public API

  thymio_variables->fwversion[0] = 11;
  thymio_variables->fwversion[1] = 0;
  thymio_variables->productId = ASEBA_PID_THYMIO2;
  thymio_variables->timerPeriod[0] = 0;
  thymio_variables->timerPeriod[1] = 0;

  // variables.sdPresent = openSDCardFile(0) ? 1 : 0;
  // if (variables.sdPresent)
  // openSDCardFile(-1);
}

// inline double distance(double x1, double y1, double z1, double x2, double y2, double z2) {
//   return sqrt((x1-x2)*(x1-x2) + (y1-y2)*(y1-y2) + (z1-z2)*(z1-z2));
// }


#define G 9.81f
#define TAP_THRESHOLD 11
// input in m^2/s

inline uint16_t aseba_acc(float a) {
  return round(std::max(std::min(a / G * 23.0f, 32.0f), -32.0f));
}

void AsebaThymio2::step(float dt) {
  // get physical variables
  //
  robot->update_sensing(dt);

  for (size_t i = 0; i < 7; i++) {
    thymio_variables->proxHorizontal[i] = robot->get_proximity_value(i);
  }
  for (size_t i = 0; i < 2; i++) {
    thymio_variables->proxGroundReflected[i] = std::clamp(
        (int)(robot->get_ground_reflected(i)), 0, 1023);
    thymio_variables->proxGroundDelta[i] = std::clamp(
        (int)(robot->get_ground_delta(i)), 0, 1023);
    // thymio_variables->proxGroundAmbient[i] = 0;
  }
  thymio_variables->motorLeftSpeed = robot->get_speed(0) * 500. / 0.166;
  thymio_variables->motorRightSpeed = robot->get_speed(1) * 500. / 0.166;

  // aseba 0 = left, aseba 1 = front, aseba 3 = up
  //
  int32_t a[3];
  for (size_t i = 0; i < 3; i++) {
    a[i] = thymio_variables->acc[i];
  }
  thymio_variables->acc[0] = aseba_acc(robot->get_acceleration(1));
  thymio_variables->acc[1] = aseba_acc(robot->get_acceleration(0));
  thymio_variables->acc[2] = aseba_acc(robot->get_acceleration(2));
  // trigger tap event
  // in the real thymio , tap is detected by the accelerometer
  // when the y or z acc value is > 11 for >= 20 steps = 1/6 s
  // See https://www.nxp.com/docs/en/data-sheet/MMA7660FC.pdf
  // and mma7660.c
  bool tap = false;
  for (size_t i = 0; i < 3; i++) {
    if (abs(a[i] - thymio_variables->acc[i]) > TAP_THRESHOLD) {
      tap = true;
      break;
    }
  }
  if (tap && !first)
    emit(EVENT_TAP);

  for (size_t i = 0; i < 5; i++) {
    // buttons enum, variables and events are all in the same order!
    int16_t v = robot->get_button(i) ? 1 : 0;
    if (v != thymio_variables->buttons[i]) {
       thymio_variables->buttons[i] = v;
       emit(EVENT_B_BACKWARD + i);
    }
  }

  for (auto & msg : robot->prox_comm_rx()) {
    for (size_t i = 0; i < 7; i++) {
      thymio_variables->proxCommPayloads[i] = (int16_t) msg.payloads[i];
      thymio_variables->proxCommIntensities[i] = (int16_t) msg.intensities[i];
    }
    thymio_variables->proxCommRx = msg.rx;
    emit(EVENT_PROX_COMM);
  }

  if (robot->received_rc_message(thymio_variables->rc5address, thymio_variables->rc5command)) {
    emit(EVENT_RC5);
  }

  thymio_variables->micIntensity = (int16_t) (255 * robot->get_mic_intensity());

  int16_t micThreshold = thymio_variables->micThreshold;
  if (micThreshold != oldMicThreshold) {
    robot->set_mic_threshold((float) micThreshold / 255.0);
  } else {
    thymio_variables->micThreshold = (int16_t) (255 * robot->get_mic_threshold());
  }
  oldMicThreshold = thymio_variables->micThreshold;
  if (oldMicThreshold > 0 && (thymio_variables->micIntensity > oldMicThreshold)) {
    emit(EVENT_MIC);
  }

  // run timers
  timer0.step(dt);
  timer1.step(dt);
  timer100Hz.step(dt);

  DynamicAsebaNode::step(dt);

  // reset a timer if its period changed
  if (thymio_variables->timerPeriod[0] != oldTimerPeriod[0]) {
    oldTimerPeriod[0] = thymio_variables->timerPeriod[0];
    timer0.setPeriod(thymio_variables->timerPeriod[0] / 1000.);
  }
  if (thymio_variables->timerPeriod[1] != oldTimerPeriod[1]) {
    oldTimerPeriod[1] = thymio_variables->timerPeriod[1];
    timer1.setPeriod(thymio_variables->timerPeriod[1] / 1000.);
  }
  uint16_t tx = thymio_variables->proxCommTx;
  // limit to 11 bits
  tx &= ((1>>11) - 1);
  if (first || oldProxCommTx == tx) {
    tx = robot->prox_comm_tx();
    tx &= ((1>>11) - 1);
    thymio_variables->proxCommTx = tx;
  } else {
    robot->set_prox_comm_tx(tx);
  }
  oldProxCommTx = thymio_variables->proxCommTx;
  if (first || oldMotorLeftTarget == thymio_variables->motorLeftTarget) {
    thymio_variables->motorLeftTarget = robot->get_target_speed(0) * 500. / 0.166;
  } else {
    robot->set_target_speed(0, double(thymio_variables->motorLeftTarget) * 0.166 / 500.);
  }
  if (first || oldMotorRightTarget == thymio_variables->motorRightTarget) {
    thymio_variables->motorRightTarget = robot->get_target_speed(1) * 500. / 0.166;
  } else {
    robot->set_target_speed(1, double(thymio_variables->motorRightTarget) * 0.166 / 500.);
  }
  oldMotorLeftTarget = thymio_variables->motorLeftTarget;
  oldMotorRightTarget = thymio_variables->motorRightTarget;

  // set physical variables
  // robot->set_target_speed(0, double(thymio_variables->motorLeftTarget) * 0.166 / 500.);
  // robot->set_target_speed(1, double(thymio_variables->motorRightTarget) * 0.166 / 500.);

  first = false;

  robot->update_actuation(dt);
}



// // array of descriptions of native functions, static so only visible in this file
//
// static const AsebaNativeFunctionDescription* nativeFunctionsDescriptions[] = {
//   ASEBA_NATIVES_STD_DESCRIPTIONS,
//   PLAYGROUND_THYMIO2_NATIVES_DESCRIPTIONS,
//   0
// };


//! Open the virtual SD card file number, if -1, close current one
// bool AsebaThymio2::openSDCardFile(int number) {
//   // close current file, ignore errors
//   if (sdCardFile.is_open()) {
//   sdCardFile.close();
//   sdCardFileNumber = -1;
//   }
//
//   // if we have to open another file
//   if (number >= 0) {
//   // path for the file
//   if (!Enki::simulatorEnvironment)
//     return false;
//   const string fileName(Enki::simulatorEnvironment->getSDFilePath(robotName, unsigned(number)));
//   // try to open file
//   sdCardFile.open(fileName.c_str(), std::ios::in | std::ios::out | std::ios::binary);
//   if (sdCardFile.fail()) {
//     // failed... maybe the file does not exist, try with trunc
//     sdCardFile.open(fileName.c_str(), std::ios::in | std::ios::out |
//                     std::ios::binary | std::ios::trunc);
//   }
//   if (sdCardFile.fail()) {
//     // still failed, then it is an error
//     return false;
//   } else {
//     sdCardFileNumber = number;
//   }
//   }
//   return true;
// }

void AsebaThymio2::timer0Timeout() {
  emit(EVENT_TIMER0);
}

void AsebaThymio2::timer1Timeout() {
  emit(EVENT_TIMER1);
}

void AsebaThymio2::timer100HzTimeout() {
  ++counter100Hz;
  emit(EVENT_MOTOR);
  if (counter100Hz % 5 == 0)
    emit(EVENT_BUTTONS);
  if (counter100Hz % 10 == 0)
    emit(EVENT_PROX);
  if (counter100Hz % 6 == 0)
    emit(EVENT_ACC);
  if (counter100Hz % 100 == 0) {
    thymio_variables->temperature = (int16_t) (10 * robot->get_temperature());
    emit(EVENT_TEMPERATURE);
  }
}

void AsebaThymio2::reset() {
  DynamicAsebaNode::reset();
  thymio_variables->fwversion[0] = 11;
  thymio_variables->fwversion[1] = 0;
  thymio_variables->productId = ASEBA_PID_THYMIO2;
  thymio_variables->timerPeriod[0] = 0;
  thymio_variables->timerPeriod[1] = 0;
  oldTimerPeriod[0] = 0;
  oldTimerPeriod[1] = 0;
  oldMicThreshold = 0;
  first = true;

  robot->reset();

  log_info("Reset Thymio Aseba node");
}
