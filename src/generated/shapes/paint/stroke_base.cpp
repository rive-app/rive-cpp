#include "generated/shapes/paint/stroke_base.hpp"
#include "shapes/paint/stroke.hpp"

using namespace rive;

Core* StrokeBase::clone() const
{
	auto cloned = new Stroke();
	cloned->copy(*this);
	return cloned;
}
