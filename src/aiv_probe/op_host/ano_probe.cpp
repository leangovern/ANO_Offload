/* ============================================================================
 * op_host/ano_probe.cpp — AnoProbe 的 OpDef（任务 12-b，M6f prod2 骨架）
 * 2 输入（params/out）+ 无 attr；InferShape 静态 {256}。
 * ========================================================================= */
#include "../op_kernel/ano_probe_tiling.h"
#include "register/op_def_registry.h"

namespace optiling {
static ge::graphStatus TilingFunc(gert::TilingContext* context)
{
    AnoProbeTilingData* tilingData = context->GetTilingData<AnoProbeTilingData>();
    tilingData->sel = 0;
    context->SetBlockDim(1);            /* 单 AIV 核（prod2 实证形态） */
    size_t* ws = context->GetWorkspaceSizes(1);
    ws[0] = 0;
    return ge::GRAPH_SUCCESS;
}
}

namespace ge {
static ge::graphStatus InferShape(gert::InferShapeContext* context)
{
    context->GetOutputShape(0)->SetDimNum(1);
    context->GetOutputShape(0)->SetDim(0, 256);
    return ge::GRAPH_SUCCESS;
}
static ge::graphStatus InferDataType(gert::InferDataTypeContext *context)
{
    context->SetOutputDataType(0, context->GetInputDataType(0));
    return ge::GRAPH_SUCCESS;
}
}

namespace ops {
class AnoProbe : public OpDef {
public:
    explicit AnoProbe(const char* name) : OpDef(name)
    {
        this->Input("params")
            .ParamType(REQUIRED)
            .DataType({ge::DT_UINT8})
            .Format({ge::FORMAT_ND})
            .UnknownShapeFormat({ge::FORMAT_ND});
        this->Output("out")
            .ParamType(REQUIRED)
            .DataType({ge::DT_UINT8})
            .Format({ge::FORMAT_ND})
            .UnknownShapeFormat({ge::FORMAT_ND});

        this->SetInferShape(ge::InferShape).SetInferDataType(ge::InferDataType);
        this->AICore().SetTiling(optiling::TilingFunc);
        this->AICore().AddConfig("ascend950");
    }
};

OP_ADD(AnoProbe);
}
