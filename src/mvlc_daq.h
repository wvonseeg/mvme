/* mvme - Mesytec VME Data Acquisition
 *
 * Copyright (C) 2016-2020 mesytec GmbH & Co. KG <info@mesytec.com>
 *
 * Author: Florian Lüke <f.lueke@mesytec.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
#ifndef __MVME_MVLC_DAQ_H__
#define __MVME_MVLC_DAQ_H__

#include "libmvme_export.h"
#include "mvlc/mvlc_qt_object.h"
#include "mvlc/mvlc_trigger_io.h"
#include "vme_config.h"

namespace mesytec
{
namespace mvme_mvlc
{

using Logger = std::function<void (const QString &)>;

// The following steps are executed:
// - disable_all_triggers
// - reset_stack_offsets
// - setup_readout_stacks
// - enable_triggers
//
// Returns std::error_codes generated by the MVLC code in case of errors.
// Exceptions thrown by internal function calls are not caught but passed on to
// the caller (e.g. vme_script::ParseError).

// Builds and uploads the readout stacks and performs the trigger I/O setup.
// Note: the readout triggers are not enabled by this function.
std::error_code LIBMVME_EXPORT
    setup_mvlc(MVLCObject &mvlc, VMEConfig &vmeConfig, Logger logger);

std::pair<std::vector<u32>, std::error_code> LIBMVME_EXPORT get_trigger_values(
    const VMEConfig &vmeConfig, Logger logger);

std::error_code LIBMVME_EXPORT
    enable_triggers(MVLCObject &mvlc, const VMEConfig &vmeConfig, Logger logger);

std::error_code LIBMVME_EXPORT
    disable_all_triggers(MVLCObject &mvlc);

std::error_code LIBMVME_EXPORT
    reset_stack_offsets(MVLCObject &mvlc);

std::error_code LIBMVME_EXPORT
    setup_readout_stacks(MVLCObject &mvlc, const VMEConfig &vmeConfig,
                         Logger logger);

std::error_code LIBMVME_EXPORT
    setup_trigger_io(MVLCObject &mvlc, VMEConfig &vmeConfig,
                     Logger logger);

// Parses the trigger io contained in the vmeconfig, updates it to handle
// periodic and externally triggered events and returns the updated TriggerIO
// structure.
mesytec::mvme_mvlc::trigger_io::TriggerIO LIBMVME_EXPORT
    update_trigger_io(const VMEConfig &vmeConfig);

void LIBMVME_EXPORT update_trigger_io_inplace(const VMEConfig &vmeConfig);

} // end namespace mvme_mvlc
} // end namespace mesytec

#endif /* __MVME_MVLC_DAQ_H__ */
