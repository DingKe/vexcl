#ifndef VEXCL_BACKEND_OPENCL_FFT_KERNELS_HPP
#define VEXCL_BACKEND_OPENCL_FFT_KERNELS_HPP

/*
The MIT License

Copyright (c) 2012-2014 Denis Demidov <dennis.demidov@gmail.com>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/**
 * \file   vexcl/backend/opencl/fft/kernels.hpp
 * \author Pascal Germroth <pascal@ensieve.org>
 * \brief  Kernel generator for FFT.
 */

#include <boost/math/constants/constants.hpp>

namespace vex {
namespace fft {

/// \cond INTERNAL
inline cl::Device qdev(const backend::command_queue& q) {
    cl::Device dev;
    q.getInfo(CL_QUEUE_DEVICE, &dev);
    return dev;
}

// Store v=b^e as components.
struct pow {
    size_t base, exponent, value;
    pow(size_t b, size_t e) : base(b), exponent(e),
        value(static_cast<size_t>(std::pow(static_cast<double>(b), static_cast<double>(e)))) {}
};

inline std::ostream &operator<<(std::ostream &o, const pow &p) {
    o << p.base;
    if(p.exponent != 1) o << '^' << p.exponent;
    return o;
}

struct kernel_call {
    bool once;
    size_t count;
    std::string desc;
    cl::Program program;
    cl::Kernel kernel;
    cl::NDRange global, local;
    kernel_call(bool o, std::string d, cl::Program p, cl::Kernel k, cl::NDRange g, cl::NDRange l) : once(o), count(0), desc(d), program(p), kernel(k), global(g), local(l) {}
};




// generates "(prefix vfrom,vfrom+1,...,vto)"
inline void param_list(backend::source_generator &o,
        std::string prefix, size_t from, size_t to, size_t step = 1)
{
    o << '(';
    for(size_t i = from ; i != to ; i += step) {
        if(i != from) o << ", ";
        o << prefix << 'v' << i;
    } o << ')';
}

template <class T, class T2>
inline void kernel_radix(backend::source_generator &o, pow radix, bool invert) {
    o << in_place_dft(radix.value, invert);

    // kernel.
    o.kernel("radix").open("(")
        .template parameter< global_ptr<const T2> >("x")
        .template parameter< global_ptr<      T2> >("y")
        .template parameter< cl_uint              >("p")
        .template parameter< cl_uint              >("threads")
    .close(")").open("{");

    o.new_line() << "const size_t i = " << o.global_id(0) << ";";
    o.new_line() << "if(i >= threads) return;";

    // index in input sequence, in 0..P-1
    o.new_line() << "const size_t k = i % p;";
    o.new_line() << "const size_t batch_offset = " << o.global_id(1) << " * threads * " << radix.value << ";";

    // read
    o.new_line() << "x += i + batch_offset;";
    for(size_t i = 0; i < radix.value; ++i)
        o.new_line() << type_name<T2>() << " v" << i << " = x[" << i << " * threads];";

    // twiddle
    o.new_line() << "if(p != 1)";
    o.open("{");
    for(size_t i = 1; i < radix.value; ++i) {
        const T alpha = -boost::math::constants::two_pi<T>() * i / radix.value;
        o.new_line() << "v" << i << " = mul(v" << i << ", twiddle("
          << "(" << type_name<T>() << ")" << std::setprecision(16) << alpha << " * k / p));";
    }
    o.close("}");

    // inplace DFT
    o.new_line() << "dft" << radix.value;
    param_list(o, "&", 0, radix.value);
    o << ";";

    // write back
    o.new_line() << "const size_t j = k + (i - k) * " << radix.value << ";";
    o.new_line() << "y += j + batch_offset;";
    for(size_t i = 0; i < radix.value; i++)
        o.new_line() << "y[" << i << " * p] = v" << i << ";";
    o.close("}");
}


template <class T>
inline void kernel_common(backend::source_generator &o, const backend::command_queue& q) {
#ifdef VEXCL_BACKEND_OPENCL
    o << "#define DEVICE\n";
#else
    o << "#define DEVICE __device__\n";
#endif
    if(std::is_same<T, cl_double>::value) {
        o << backend::standard_kernel_header(q)
          << "typedef double real_t;\n"
          << "typedef double2 real2_t;\n";
    } else {
        o << "typedef float real_t;\n"
          << "typedef float2 real2_t;\n";
    }
}

// Return A*B (complex multiplication)
template <class T2>
inline void mul_code(backend::source_generator &o, bool invert) {
    o.function<T2>("mul").open("(")
        .template parameter<T2>("a")
        .template parameter<T2>("b")
    .close(")").open("{");

    if(invert) { // conjugate b
        o.new_line() << type_name<T2>() << " r = {"
            "a.x * b.x + a.y * b.y, "
            "a.y * b.x - a.x * b.y};";
    } else {
        o.new_line() << type_name<T2>() << " r = {"
            "a.x * b.x - a.y * b.y, "
            "a.y * b.x + a.x * b.y};";
    }

    o.new_line() << "return r;";
    o.close("}");
}

// A * exp(alpha * I) == A  * (cos(alpha) + I * sin(alpha))
// native_cos(), native_sin() is a *lot* faster than sincos, on nVidia.
template <class T, class T2>
inline void twiddle_code(backend::source_generator &o) {
    o.function<T2>("twiddle").open("(")
        .template parameter<T>("alpha")
    .close(")").open("{");

    if(std::is_same<T, cl_double>::value) {
        // use sincos with double since we probably want higher precision
#ifdef VEXCL_BACKEND_OPENCL
        o.new_line() << type_name<T>() << " cs, sn = sincos(alpha, &cs);";
#else
        o.new_line() << type_name<T>() << " sn, cs;";
        o.new_line() << "sincos(alpha, &sn, &cs);";
#endif
        o.new_line() << type_name<T2>() << " r = {cs, sn};";
    } else {
        // use native with float since we probably want higher performance
#ifdef VEXCL_BACKEND_OPENCL
        o.new_line() << type_name<T2>() << " r = {"
            "native_cos(alpha), native_sin(alpha)};";
#else
        o.new_line() << type_name<T>() << " sn, cs;";
        o.new_line() << "__sincosf(alpha, &sn, &cs);";
        o.new_line() << type_name<T2>() << " r = {cs, sn};";
#endif
    }

    o.new_line() << "return r;";
    o.close("}");
}


template <class T, class T2>
inline kernel_call radix_kernel(
        bool once, const backend::command_queue &queue, size_t n, size_t batch,
        bool invert, pow radix, size_t p,
        const backend::device_vector<T2> &in,
        const backend::device_vector<T2> &out
        )
{
    backend::source_generator o;
    o << std::setprecision(25);
    const auto device = qdev(queue);
    kernel_common<T>(o, queue);
    mul_code<T2>(o, invert);
    twiddle_code<T, T2>(o);

    const size_t m = n / radix.value;
    kernel_radix<T, T2>(o, radix, invert);

    auto program = backend::build_sources(queue, o.str(), "-cl-mad-enable -cl-fast-relaxed-math");
    cl::Kernel kernel(program, "radix");
    kernel.setArg(0, in);
    kernel.setArg(1, out);
    kernel.setArg(2, static_cast<cl_uint>(p));
    kernel.setArg(3, static_cast<cl_uint>(m));

    const size_t wg_mul = kernel.getWorkGroupInfo<CL_KERNEL_PREFERRED_WORK_GROUP_SIZE_MULTIPLE>(device);
    //const size_t max_cu = device.getInfo<CL_DEVICE_MAX_COMPUTE_UNITS>();
    //const size_t max_wg = device.getInfo<CL_DEVICE_MAX_WORK_GROUP_SIZE>();
    size_t wg = wg_mul;
    //while(wg * max_cu < max_wg) wg += wg_mul;
    //wg -= wg_mul;
    const size_t threads = alignup(m, wg);

    std::ostringstream desc;
    desc << "dft{r=" << radix << ", p=" << p << ", n=" << n << ", batch=" << batch << ", threads=" << m << "(" << threads << "), wg=" << wg << "}";

    return kernel_call(once, desc.str(), program, kernel, cl::NDRange(threads, batch), cl::NDRange(wg, 1));
}


template <class T, class T2>
inline kernel_call transpose_kernel(
        const backend::command_queue &queue, size_t width, size_t height,
        const backend::device_vector<T2> &in,
        const backend::device_vector<T2> &out
        )
{
    backend::source_generator o;
    const auto dev = qdev(queue);
    kernel_common<T>(o, queue);

    // determine max block size to fit into local memory/workgroup
    size_t block_size = 128;
    {
        const auto local_size = dev.getInfo<CL_DEVICE_LOCAL_MEM_SIZE>();
        const auto workgroup = dev.getInfo<CL_DEVICE_MAX_WORK_GROUP_SIZE>();
        while(block_size * block_size * sizeof(T) * 2 > local_size) block_size /= 2;
        while(block_size * block_size > workgroup) block_size /= 2;
    }

    // from NVIDIA SDK.
    o.kernel("transpose").open("(")
        .template parameter< global_ptr<const T2> >("input")
        .template parameter< global_ptr<      T2> >("output")
        .template parameter< cl_uint              >("width")
        .template parameter< cl_uint              >("height")
    .close(")").open("{");

    o.new_line() << "const size_t global_x = " << o.global_id(0) << ";";
    o.new_line() << "const size_t global_y = " << o.global_id(1) << ";";
    o.new_line() << "const size_t local_x  = " << o.local_id(0)  << ";";
    o.new_line() << "const size_t local_y  = " << o.local_id(1)  << ";";
    o.new_line() << "const size_t group_x  = " << o.group_id(0)  << ";";
    o.new_line() << "const size_t group_y  = " << o.group_id(1)  << ";";
    o.new_line() << "const size_t block_size = " << block_size << ";";
    o.new_line() << "const size_t target_x = local_y + group_y * block_size;";
    o.new_line() << "const size_t target_y = local_x + group_x * block_size;";
    o.new_line() << "const bool range = global_x < width && global_y < height;";

    // local memory
    {
        std::ostringstream s;
        s << "block[" << block_size * block_size << "]";
        o.smem_static_var(type_name<T2>(), s.str());
    }

    // copy from input to local memory
    o.new_line() << "if(range) "
        << "block[local_x + local_y * block_size] = input[global_x + global_y * width];";

    // wait until the whole block is filled
    o.new_line() << "barrier(CLK_LOCAL_MEM_FENCE);";

    // transpose local block to target
    o.new_line() << "if(range) "
      << "output[target_x + target_y * height] = block[local_x + local_y * block_size];";

    o.close("}");

    auto program = backend::build_sources(queue, o.str());
    cl::Kernel kernel(program, "transpose");
    kernel.setArg(0, in);
    kernel.setArg(1, out);
    kernel.setArg(2, static_cast<cl_uint>(width));
    kernel.setArg(3, static_cast<cl_uint>(height));

    // range multiple of wg size, last block maybe not completely filled.
    size_t r_w = alignup(width, block_size);
    size_t r_h = alignup(height, block_size);

    std::ostringstream desc;
    desc << "transpose{"
         << "w=" << width << "(" << r_w << "), "
         << "h=" << height << "(" << r_h << "), "
         << "bs=" << block_size << "}";

    return kernel_call(false, desc.str(), program, kernel, cl::NDRange(r_w, r_h),
        cl::NDRange(block_size, block_size));
}



template <class T, class T2>
inline kernel_call bluestein_twiddle(
        const backend::command_queue &queue, size_t n, bool inverse,
        const backend::device_vector<T2> &out
        )
{
    backend::source_generator o;
    kernel_common<T>(o, queue);
    twiddle_code<T, T2>(o);

    o.kernel("bluestein_twiddle").open("(")
        .template parameter< global_ptr<T2> >("output")
    .close(")").open("{");

    o.new_line() << "const size_t x = " << o.global_id(0) << ";";
    o.new_line() << "const size_t n = " << o.global_size(0) << ";";

    o.new_line() << "const size_t xx = ((ulong)x * x) % (2 * n);";
    o.new_line() << "output[x] = twiddle(" << std::setprecision(16)
        << (inverse ? 1 : -1) * boost::math::constants::pi<T>()
        << " * xx / n);";

    o.close("}");

    auto program = backend::build_sources(queue, o.str());
    cl::Kernel kernel(program, "bluestein_twiddle");
    kernel.setArg(0, out);

    std::ostringstream desc;
    desc << "bluestein_twiddle{n=" << n << ", inverse=" << inverse << "}";
    return kernel_call(true, desc.str(), program, kernel, cl::NDRange(n), cl::NullRange);
}

template <class T, class T2>
inline kernel_call bluestein_pad_kernel(
        const backend::command_queue &queue, size_t n, size_t m,
        const backend::device_vector<T2> &in,
        const backend::device_vector<T2> &out
        )
{
    backend::source_generator o;
    kernel_common<T>(o, queue);

    o.function<T2>("conj").open("(")
        .template parameter<T2>("v")
    .close(")").open("{");
    o.new_line() << type_name<T2>() << " r = {v.x, -v.y};";
    o.new_line() << "return r;";
    o.close("}");

    o.kernel("bluestein_pad_kernel").open("(")
        .template parameter< global_ptr<const T2> >("input")
        .template parameter< global_ptr<      T2> >("output")
        .template parameter< cl_uint              >("n")
        .template parameter< cl_uint              >("m")
    .close(")").open("{");
    o.new_line() << "const size_t x = " << o.global_id(0) << ";";
    o.new_line() << "if(x < n || m - x < n)";
    o.open("{").new_line()  << "output[x] = conj(input[min(x, m - x)]);";
    o.close("}").new_line() << "else";
    o.open("{").new_line()  << type_name<T2>() << " r = {0,0};";
    o.new_line() << "output[x] = r;";
    o.close("}").close("}");

    auto program = backend::build_sources(queue, o.str());
    cl::Kernel kernel(program, "bluestein_pad_kernel");
    kernel.setArg(0, in);
    kernel.setArg(1, out);
    kernel.setArg(2, static_cast<cl_uint>(n));
    kernel.setArg(3, static_cast<cl_uint>(m));

    std::ostringstream desc;
    desc << "bluestein_pad_kernel{n=" << n << ", m=" << m << "}";
    return kernel_call(true, desc.str(), program, kernel, cl::NDRange(m), cl::NullRange);
}

template <class T, class T2>
inline kernel_call bluestein_mul_in(
        const backend::command_queue &queue, bool inverse, size_t batch,
        size_t radix, size_t p, size_t threads, size_t stride,
        const backend::device_vector<T2> &data,
        const backend::device_vector<T2> &exp,
        const backend::device_vector<T2> &out
        )
{
    backend::source_generator o;
    kernel_common<T>(o, queue);
    mul_code<T2>(o, false);
    twiddle_code<T, T2>(o);

    o << "__kernel void bluestein_mul_in("
      << "__global const real2_t *data, __global const real2_t *exp, __global real2_t *output, "
      << "uint radix, uint p, uint out_stride) {\n"
      << "  const size_t\n"
      << "    thread = get_global_id(0), threads = get_global_size(0),\n"
      << "    batch = get_global_id(1),\n"
      << "    element = get_global_id(2);\n"
      << "  if(element < out_stride) {\n"
      << "    const size_t\n"
      << "      in_off = thread + batch * radix * threads + element * threads,\n"
      << "      out_off = thread * out_stride + batch * out_stride * threads + element;\n"
      << "    if(element < radix) {\n"
      << "      real2_t w = exp[element];"
      << "      if(p != 1) {\n"
      << "        const int sign = " << (inverse ? "+1" : "-1") << ";\n"
      << "        ulong a = (ulong)element * (thread % p);\n"
      << "        ulong b = (ulong)radix * p;\n"
      << "        real2_t t = twiddle(2 * sign * M_PI * (a % (2 * b)) / b);\n"
      << "        w = mul(w, t);\n"
      << "      }\n"
      << "      output[out_off] = mul(data[in_off], w);\n"
      << "    } else\n"
      << "      output[out_off] = (real2_t)(0,0);"
      << "  }\n"
      << "}\n";

    auto program = backend::build_sources(queue, o.str());
    cl::Kernel kernel(program, "bluestein_mul_in");
    kernel.setArg(0, data);
    kernel.setArg(1, exp);
    kernel.setArg(2, out);
    kernel.setArg(3, static_cast<cl_uint>(radix));
    kernel.setArg(4, static_cast<cl_uint>(p));
    kernel.setArg(5, static_cast<cl_uint>(stride));

    const size_t wg = kernel.getWorkGroupInfo<CL_KERNEL_PREFERRED_WORK_GROUP_SIZE_MULTIPLE>(qdev(queue));
    const size_t stride_pad = alignup(stride, wg);

    std::ostringstream desc;
    desc << "bluestein_mul_in{batch=" << batch << ", radix=" << radix << ", p=" << p << ", threads=" << threads << ", stride=" << stride << "(" << stride_pad << "), wg=" << wg << "}";
    return kernel_call(false, desc.str(), program, kernel, cl::NDRange(threads, batch, stride_pad), cl::NDRange(1, 1, wg));
}

template <class T, class T2>
inline kernel_call bluestein_mul_out(
        const backend::command_queue &queue, size_t batch, size_t p,
        size_t radix, size_t threads, size_t stride,
        const backend::device_vector<T2> &data,
        const backend::device_vector<T2> &exp,
        const backend::device_vector<T2> &out
        )
{
    backend::source_generator o;
    kernel_common<T>(o, queue);
    mul_code<T2>(o, false);

    o << "__kernel void bluestein_mul_out("
      << "__global const real2_t *data, __global const real2_t *exp, __global real2_t *output, "
      << "real_t div, uint p, uint in_stride, uint radix) {\n"
      << "  const size_t\n"
      << "    i = get_global_id(0), threads = get_global_size(0),\n"
      << "    b = get_global_id(1),\n"
      << "    l = get_global_id(2);\n"
      << "  if(l < radix) {\n"
      << "    const size_t\n"
      << "      k = i % p,\n"
      << "      j = k + (i - k) * radix,\n"
      << "      in_off = i * in_stride + b * in_stride * threads + l,\n"
      << "      out_off = j + b * threads * radix + l * p;\n"
      << "    output[out_off] = mul(data[in_off] * div, exp[l]);\n"
      << "  }\n"
      << "}\n";

    auto program = backend::build_sources(queue, o.str());
    cl::Kernel kernel(program, "bluestein_mul_out");
    kernel.setArg(0, data);
    kernel.setArg(1, exp);
    kernel.setArg(2, out);
    kernel.setArg<T>(3, static_cast<T>(1) / stride);
    kernel.setArg(4, static_cast<cl_uint>(p));
    kernel.setArg(5, static_cast<cl_uint>(stride));
    kernel.setArg(6, static_cast<cl_uint>(radix));

    const size_t wg = kernel.getWorkGroupInfo<CL_KERNEL_PREFERRED_WORK_GROUP_SIZE_MULTIPLE>(qdev(queue));
    const size_t radix_pad = alignup(radix, wg);

    std::ostringstream desc;
    desc << "bluestein_mul_out{r=" << radix << "(" << radix_pad << "), wg=" << wg << ", batch=" << batch << ", p=" << p << ", thr=" << threads << ", stride=" << stride << "}";
    return kernel_call(false, desc.str(), program, kernel, cl::NDRange(threads, batch, radix_pad), cl::NDRange(1, 1, wg));
}

template <class T, class T2>
inline kernel_call bluestein_mul(
        const backend::command_queue &queue, size_t n, size_t batch,
        const backend::device_vector<T2> &data,
        const backend::device_vector<T2> &exp,
        const backend::device_vector<T2> &out
        )
{
    backend::source_generator o;
    kernel_common<T>(o, queue);
    mul_code<T2>(o, false);

    o << "__kernel void bluestein_mul("
      << "__global const real2_t *data, __global const real2_t *exp, __global real2_t *output, uint stride) {\n"
      << "  const size_t x = get_global_id(0), y = get_global_id(1);\n"
      << "  if(x < stride) {\n"
      << "    const size_t off = x + stride * y;"
      << "    output[off] = mul(data[off], exp[x]);\n"
      << "  }\n"
      << "}\n";

    auto program = backend::build_sources(queue, o.str());
    cl::Kernel kernel(program, "bluestein_mul");
    kernel.setArg(0, data);
    kernel.setArg(1, exp);
    kernel.setArg(2, out);
    kernel.setArg(3, static_cast<cl_uint>(n));

    const size_t wg = kernel.getWorkGroupInfo<CL_KERNEL_PREFERRED_WORK_GROUP_SIZE_MULTIPLE>(qdev(queue));
    const size_t threads = alignup(n, wg);

    std::ostringstream desc;
    desc << "bluestein_mul{n=" << n << "(" << threads << "), wg=" << wg << ", batch=" << batch << "}";
    return kernel_call(false, desc.str(), program, kernel, cl::NDRange(threads, batch), cl::NDRange(wg, 1));
}

/// \endcond

} // namespace fft
} // namespace vex


#endif
