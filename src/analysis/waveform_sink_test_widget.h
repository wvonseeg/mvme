#ifndef E1A736A6_79F0_4307_8574_A82B6B682A64
#define E1A736A6_79F0_4307_8574_A82B6B682A64

#include "analysis.h"
#include "histo_ui.h"
#include "libmvme_export.h"

namespace analysis
{

class LIBMVME_EXPORT WaveformSinkDontKnowYetWidget: public histo_ui::PlotWidget
{
    Q_OBJECT
    public:
        WaveformSinkDontKnowYetWidget(
            const std::shared_ptr<analysis::WaveformSink> &sink,
            AnalysisServiceProvider *asp,
            QWidget *parent = nullptr);

        ~WaveformSinkDontKnowYetWidget() override;

    public slots:
        void replot() override;

    private:
        struct Private;
        std::unique_ptr<Private> d;
};

}

#endif /* E1A736A6_79F0_4307_8574_A82B6B682A64 */
