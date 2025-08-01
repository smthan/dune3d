#include "tool_common.hpp"

namespace dune3d {

class IConstraintDatum;

class ToolToggleMeasurement : public ToolCommon {
public:
    using ToolCommon::ToolCommon;

    ToolResponse begin(const ToolArgs &args) override;
    ToolResponse update(const ToolArgs &args) override;
    bool is_specific() override
    {
        return true;
    }

    CanBegin can_begin() override;

private:
    std::set<IConstraintDatum *> get_constraints();
};

} // namespace dune3d
