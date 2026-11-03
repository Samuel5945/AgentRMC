#pragma once

#include <QPushButton>

class QPainter;

// 声波条：idle 静态波形，active 时随电平+时间相位跳动。
// 语音键与语音主界面麦克风内盘共用这一种图形语言。
void paintWaveBars(QPainter &painter, const QRectF &area, double level,
                   bool active, const QColor &color);

// 语音键：监听关 = 灰底提示先开监听；监听开 = 主题渐变底，点击说话/再点结束；
// 录音中波形随麦克风电平跳动。自绘不吃 QSS 背景，颜色全走主题令牌。
class WaveformButton final : public QPushButton {
    Q_OBJECT

public:
    explicit WaveformButton(QWidget *parent = nullptr);

    void setListening(bool on);
    void setRecording(bool on);
    void setLevel(double level);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    bool listening_ = false;
    bool recording_ = false;
    double level_ = 0.0;
};
