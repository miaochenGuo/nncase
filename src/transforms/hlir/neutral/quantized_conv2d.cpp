/* Copyright 2019-2020 Canaan Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <algorithm>
#include <hlir/ops/conv2d.h>
#include <hlir/ops/dequantize.h>
#include <hlir/ops/quantize.h>
#include <hlir/transforms/neutral/quantized_conv2d.h>
#include <hlir/visitor.h>
#include <xtensor/xview.hpp>

using namespace nncase;
using namespace nncase::hlir;
using namespace nncase::hlir::transforms;

namespace
{
auto quantize_weights(quantizer &quantizer, conv2d &conv)
{
    auto weights = conv.weights();
    xt::xtensor<uint8_t, 4> q_weights(conv.weights().shape());
    auto total_range = quantizer.fixup_range(quantizer.get_range(weights.begin(), weights.end()));
    auto q_p = quantizer.get_quant_param(total_range, 8);

    auto out_it = q_weights.begin();
    for (auto w : weights)
        *out_it++ = (uint8_t)std::clamp((int32_t)std::round(w * q_p.scale + q_p.zero_point), 0, 255);
    return std::make_tuple(q_p, std::move(q_weights));
}

auto quantize_bn_act(quantizer &quantizer, conv2d &conv, float sa, const quant_param_t &yq_p)
{
    auto q_bias = xt::xtensor<int32_t, 1>::from_shape({ (size_t)conv.output_channels() });
    auto &bias = conv.bias();
    auto so = yq_p.scale / sa;
    auto bn_mul = quantizer.get_fixed_mul(so, 32, 255, true);
    assert(bn_mul.shift > 0);

    for (size_t i = 0; i < bias.size(); i++)
        q_bias[i] = (int32_t)std::round(bias[i] * sa);

    return std::make_tuple(std::move(q_bias), bn_mul);
}
}

bool quantized_conv2d_transform::on_try_match(node &node, transform_context &context)
{
    if (auto conv = node_cast<conv2d>(node))
    {
        if (conv->input().connection()->attributes() & cnctr_attr_need_quantize
            && conv->output().attributes() & cnctr_attr_need_quantize)
        {
            context.inputs.emplace_back(&conv->input());
            context.outputs.emplace_back(&conv->output());

            context.matched_nodes.emplace_back(conv);
            return true;
        }
    }

    return false;
}

void quantized_conv2d_transform::process(transform_context &context)
{
    auto &output = *context.inputs[0]->connection();
    auto inputs = context.outputs[0]->connections();
    auto &old_conv = static_cast<conv2d &>(*context.matched_nodes[0]);

    auto iq_p = quantizer_.get_quant_param(quantizer_.get(output), 8);
    auto [wq_p, q_weights] = quantize_weights(quantizer_, old_conv);
    auto yq_p = quantizer_.get_quant_param(quantizer_.get(old_conv.output()), 8);
    auto sa = iq_p.scale * wq_p.scale;
    auto [q_bias, act] = quantize_bn_act(quantizer_, old_conv, sa, yq_p);

    auto q = context.graph.emplace<quantize>(output.shape(), iq_p);
    q->name(output.owner().name() + "/quantize");
    auto conv = context.graph.emplace<quantized_conv2d>(q->output().shape(), std::move(q_weights), std::move(q_bias), old_conv.groups(), old_conv.padding_h(),
        old_conv.padding_w(), old_conv.stride_h(), old_conv.stride_w(), old_conv.dilation_h(), old_conv.dilation_w(), -iq_p.zero_point, -wq_p.zero_point,
        act.rounded_mul(), act.shift, yq_p.zero_point);
    conv->name(old_conv.name());
    auto deq = context.graph.emplace<dequantize>(conv->output().shape(), yq_p);
    deq->name(old_conv.name() + "/dequantize");
    link(old_conv.output(), deq->output(), &quantizer_);

    conv->input().connect(q->output());
    deq->input().connect(conv->output());

    q->input().connect(output);
    for (auto &in : dup(inputs))
        in->connect(deq->output());
}
