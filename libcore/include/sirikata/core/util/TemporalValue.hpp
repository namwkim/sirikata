/*  Sirikata Utilities -- Math Library
 *  TemporalValue.hpp
 *
 *  Copyright (c) 2009, Daniel Reiter Horn
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are
 *  met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name of Sirikata nor the names of its contributors may
 *    be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
 * OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef _TEMPORAL_VALUE_HPP_
#define _TEMPORAL_VALUE_HPP_

#include "Time.hpp"

namespace Sirikata {

template <typename Value, typename TimeType>
class TemporalValueBase {
public:
    typedef TimeType Time;
    Value mCurrentValue;
    TimeType mWhen;
public:
    TemporalValueBase(const TimeType &when, const Value&l)
       : mCurrentValue(l),
         mWhen(when){}
    Value extrapolate(const TimeType &t) const {
        return mCurrentValue.extrapolate(t-mWhen);
    }
    TimeType time() const{
        return mWhen;
    }
    const Value& value() const {
        return mCurrentValue;
    }
    void updateValue(const TimeType&t,const Value&l) {
        mCurrentValue=l;
        mWhen=t;
    }
};

template <typename Value>
class TemporalValue : public TemporalValueBase<Value, Time> {
public:
    TemporalValue()
     : TemporalValueBase<Value, Time>( Time(Time::null()), Value() )
    {}
    TemporalValue(const Time& when, const Value& l)
     : TemporalValueBase<Value, Time>(when, l)
    {}
}; // class TemporalValue

}
#endif
