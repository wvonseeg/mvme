#ifndef __MVME_MVLC_VME_DEBUG_WIDGET_H__
#define __MVME_MVLC_VME_DEBUG_WIDGET_H__

#include <QWidget>
#include <memory>
#include "mvlc/mvlc_qt_object.h"

namespace Ui
{
    class VMEDebugWidget;
}

namespace mesytec
{
namespace mvlc
{

class VMEDebugWidget: public QWidget
{
    Q_OBJECT

    public:
        VMEDebugWidget(MVLCObject *mvlc, QWidget *parent = 0);
        virtual ~VMEDebugWidget();

    private:
        std::unique_ptr<Ui::VMEDebugWidget> ui;
        MVLCObject *m_mvlc;
};

} // end namespace mvlc
} // end namespace mesytec

#endif /* __MVME_MVLC_VME_DEBUG_WIDGET_H__ */
