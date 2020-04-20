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
#ifndef __MVME_MVLC_IMPL_SUPPORT_H__
#define __MVME_MVLC_IMPL_SUPPORT_H__

#include <array>
#include "typedefs.h"

namespace mesytec
{
namespace mvme_mvlc
{

template<size_t Capacity>
struct ReadBuffer
{
    std::array<u8, Capacity> data;
    u8 *first;
    u8 *last;

    ReadBuffer() { clear(); }
    size_t size() const { return last - first; }
    size_t free() const { return Capacity - size(); }
    size_t capacity() const { return Capacity; }
    void clear() { first = last = data.data(); }
};

} // end namespace mvme_mvlc
} // end namespace mesytec

#endif /* __MVME_MVLC_IMPL_SUPPORT_H__ */
