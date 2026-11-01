#include "ui/PetSprite.h"

#include "core/ThemeManager.h"

#include <cmath>

namespace {
// 浮动/摆动周期（ms）：比帧率慢一档，观感是「呼吸」而不是「抽搐」。
constexpr int kBobPeriodMs = 2800;
constexpr int kTiltPeriodMs = 3600;
constexpr double kPi = 3.14159265358979323846;
}  // namespace

PetSprite::PetSprite(QObject *parent) : QObject(parent) {
    clock_.start();
}

void PetSprite::applyTheme(const Theme &theme) {
    states_ = theme.petStates;
    pixmaps_.clear();
    elapsedMs_ = 0;
}

void PetSprite::setState(const QString &state) {
    if (state_ == state) {
        return;
    }
    state_ = state;
}

bool PetSprite::hasFrames() const { return resolved() != nullptr; }

const ThemePetState *PetSprite::resolved() const {
    if (states_.isEmpty()) {
        return nullptr;
    }
    auto it = states_.constFind(state_);
    if (it != states_.constEnd()) {
        return &it.value();
    }
    it = states_.constFind(QStringLiteral("idle"));
    return it != states_.constEnd() ? &it.value() : nullptr;
}

void PetSprite::advance(int ms) {
    elapsedMs_ = (elapsedMs_ + ms) % (kBobPeriodMs * kTiltPeriodMs);
}

bool PetSprite::current(QPixmap *pixmap, qreal *rotationDeg, qreal *dy) const {
    const ThemePetState *state = resolved();
    if (state == nullptr) {
        return false;
    }
    const int frameMs = qMax(1, 1000 / qMax(1, state->fps));
    const int index = int((elapsedMs_ / frameMs) % state->frames.size());
    const QString &path = state->frames.at(index);
    QPixmap frame = pixmaps_.value(path);
    if (frame.isNull()) {
        const QImage image = ThemeManager::cachedImage(path);
        if (image.isNull()) {
            return false;
        }
        frame = QPixmap::fromImage(image);
        pixmaps_.insert(path, frame);
    }
    // 时间基相位：浮动走正弦（平滑上下），摆动带一点相位差避免机械感。
    const double bobPhase = 2.0 * kPi * double(elapsedMs_ % kBobPeriodMs) /
                            double(kBobPeriodMs);
    const double tiltPhase = 2.0 * kPi * double(elapsedMs_ % kTiltPeriodMs) /
                             double(kTiltPeriodMs);
    if (dy != nullptr) {
        *dy = state->bob > 0 ? std::sin(bobPhase) * state->bob : 0.0;
    }
    if (rotationDeg != nullptr) {
        *rotationDeg = state->tilt > 0 ? std::sin(tiltPhase) * state->tilt : 0.0;
    }
    if (pixmap != nullptr) {
        *pixmap = frame;
    }
    return true;
}
