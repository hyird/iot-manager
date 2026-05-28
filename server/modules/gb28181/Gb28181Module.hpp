#pragma once

#include <memory>
#include <string>
#include <drogon/drogon.h>

class Gb28181Module {
public:
    static Gb28181Module& instance();

    Gb28181Module(const Gb28181Module&) = delete;
    Gb28181Module& operator=(const Gb28181Module&) = delete;

    void initialize();
    void start();
    void stop();
    drogon::Task<> startCoro();
    drogon::Task<> stopCoro();

    bool enabled() const;
    bool started() const;
    const std::string& lastError() const;

private:
    Gb28181Module();
    ~Gb28181Module();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};
