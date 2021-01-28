#ifndef __MVME_MVLC_TRIGGER_IO_SIM_UI_H__
#define __MVME_MVLC_TRIGGER_IO_SIM_UI_H__

#include <memory>
#include <QDebug>
#include <QMetaType>
#include <QWidget>

#include "mvlc/trigger_io_sim.h"

namespace mesytec
{
namespace mvme_mvlc
{
namespace trigger_io
{

enum class PinPosition
{
    Input,
    Output
};

struct PinAddress
{
    PinAddress() {}

    PinAddress(const UnitAddress &unit_, const PinPosition &pos_)
        : unit(unit_)
        , pos(pos_)
    {}

    PinAddress(const PinAddress &) = default;
    PinAddress &operator=(const PinAddress &) = default;

    inline bool operator==(const PinAddress &o) const
    {
        return unit == o.unit && pos == o.pos;
    }

    inline bool operator!=(const PinAddress &o) const
    {
        return !(*this == o);
    }

    UnitAddress unit = { 0, 0, 0 };
    PinPosition pos = PinPosition::Input;
};

class TraceSelectWidget: public QWidget
{
    Q_OBJECT

    signals:
        void selectionChanged(const QVector<PinAddress> &selection);

    public:
        TraceSelectWidget(QWidget *parent = nullptr);
        ~TraceSelectWidget() override;

        void setTriggerIO(const TriggerIO &trigIO);
        void setSelection(const QVector<PinAddress> &selection);

    private:
        struct Private;
        std::unique_ptr<Private> d;
};

} // end namespace trigger_io
} // end namespace mvme_mvlc
} // end namespace mesytec

Q_DECLARE_METATYPE(mesytec::mvme_mvlc::trigger_io::PinAddress);

QDataStream &operator<<(
    QDataStream &out, const mesytec::mvme_mvlc::trigger_io::PinAddress &pin);
QDataStream &operator>>(
    QDataStream &in, mesytec::mvme_mvlc::trigger_io::PinAddress &pin);

QDebug operator<<(
    QDebug dbg, const mesytec::mvme_mvlc::trigger_io::PinAddress &pin);

#endif /* __MVME_MVLC_TRIGGER_IO_SIM_UI_H__ */
