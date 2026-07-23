#include "PIDController.h"

PIDController::PIDController()
    : kp_(20.0), ki_(0.5), kd_(1.0), setpoint_(60.0),
      integral_(0.0), prevError_(0.0), firstRun_(true) {}

void PIDController::configure(double kp, double ki, double kd, double setpoint) {
    kp_ = kp;
    ki_ = ki;
    kd_ = kd;
    setpoint_ = setpoint;
    reset();
}

void PIDController::setSetpoint(double setpoint) {
    setpoint_ = setpoint;
}

void PIDController::reset() {
    integral_ = 0.0;
    prevError_ = 0.0;
    firstRun_ = true;
}

double PIDController::compute(double currentTemperature, double dtSeconds) {
    if (dtSeconds <= 0.0) dtSeconds = 0.001; // защита от деления на ноль/некорректного dt

    double error = setpoint_ - currentTemperature;

    // Предварительный (без интеграла) выход, чтобы понять, не в насыщении ли мы —
    // если да, не накапливаем интеграл дальше в ту же сторону (anti-windup).
    double derivative = firstRun_ ? 0.0 : (error - prevError_) / dtSeconds;

    double tentativeIntegral = integral_ + error * dtSeconds;
    double output = kp_ * error + ki_ * tentativeIntegral + kd_ * derivative;

    bool saturatedHigh = output > OUTPUT_MAX;
    bool saturatedLow  = output < OUTPUT_MIN;

    if (!( (saturatedHigh && error > 0) || (saturatedLow && error < 0) )) {
        // интеграл копим только если это не усугубляет насыщение
        integral_ = tentativeIntegral;
    }

    output = kp_ * error + ki_ * integral_ + kd_ * derivative;

    if (output > OUTPUT_MAX) output = OUTPUT_MAX;
    if (output < OUTPUT_MIN) output = OUTPUT_MIN;

    prevError_ = error;
    firstRun_ = false;

    return output;
}
