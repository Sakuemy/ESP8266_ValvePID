#pragma once
/*
 * PIDController.h
 * -----------------------------------------------------------------
 * Компактный собственный ПИД-регулятор (без внешних зависимостей),
 * с защитой от интегрального насыщения (anti-windup) и ограничением
 * выходного сигнала диапазоном [0..100] % открытия крана.
 * -----------------------------------------------------------------
 */

#include "Config.h"

class PIDController {
public:
    PIDController();

    void configure(double kp, double ki, double kd, double setpoint);
    void setSetpoint(double setpoint);
    void reset(); // сбросить интегральную составляющую и историю (при резких изменениях настроек)

    // Вызывать раз в PID_COMPUTE_INTERVAL_MS. dtSeconds - фактический интервал в секундах.
    // Возвращает требуемый % открытия крана (0..100).
    double compute(double currentTemperature, double dtSeconds);

    double getSetpoint() const { return setpoint_; }
    double getKp() const { return kp_; }
    double getKi() const { return ki_; }
    double getKd() const { return kd_; }

private:
    double kp_, ki_, kd_;
    double setpoint_;
    double integral_;
    double prevError_;
    bool firstRun_;

    static constexpr double OUTPUT_MIN = 0.0;
    static constexpr double OUTPUT_MAX = 100.0;
};
