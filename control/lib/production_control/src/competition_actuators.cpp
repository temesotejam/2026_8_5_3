#include "competition_actuators.h"
#include <cmath>
namespace production_control { namespace { float clampf(float v,float lo,float hi){return v<lo?lo:(v>hi?hi:v);} }
ServoMapper::ServoMapper(const ServoTuning&t):tuning_(t){reset();}
bool ServoMapper::valid(const ServoTuning&t){return std::isfinite(t.minUs)&&std::isfinite(t.neutralUs)&&std::isfinite(t.maxUs)&&t.minUs>=500.0f&&t.maxUs<=2500.0f&&t.minUs<t.neutralUs&&t.neutralUs<t.maxUs;}
void ServoMapper::reset(){previousUs_=valid(tuning_)?static_cast<uint16_t>(tuning_.neutralUs+.5f):1500;}
ServoResult ServoMapper::map(float normalized){ServoResult r{};r.finite=std::isfinite(normalized)&&valid(tuning_);if(!r.finite){r.pulseUs=previousUs_;r.clamped=true;return r;}if(tuning_.reversed)normalized=-normalized;const float constrained=clampf(normalized,-1.0f,1.0f);r.clamped=constrained!=normalized;const float target=constrained>=0?tuning_.neutralUs+constrained*(tuning_.maxUs-tuning_.neutralUs):tuning_.neutralUs+constrained*(tuning_.neutralUs-tuning_.minUs);previousUs_=static_cast<uint16_t>(target+.5f);r.pulseUs=previousUs_;return r;}
void DutyRamp::setTarget(float duty){target_=std::isfinite(duty)?clampf(duty,0.0f,maximum_):0.0f;}
float DutyRamp::step(float dt){if(!std::isfinite(dt)||dt<=0)return applied_;dt=clampf(dt,0.0f,.10f);const float seconds=target_>applied_?riseSeconds_:fallSeconds_;const float maxDelta=seconds>0?maximum_*dt/seconds:maximum_;applied_=clampf(target_,applied_-maxDelta,applied_+maxDelta);return applied_;}
}
