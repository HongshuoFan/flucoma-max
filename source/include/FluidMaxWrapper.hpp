#pragma once

#include <ext.h>
#include <ext_obex.h>
#include <ext_obex_util.h>
#include <commonsyms.h>
#include <z_dsp.h>

#include <clients/common/OfflineClient.hpp>
#include <clients/common/FluidBaseClient.hpp>
#include <clients/common/ParameterTypes.hpp>

#include <MaxBufferAdaptor.hpp> 

#include <tuple>
#include <utility>

namespace fluid {
namespace client {

namespace impl {
  
template <typename Client, typename T, size_t N> struct Setter;
template <typename Client, typename T, size_t N> struct Getter;

/// Specialisations for managing the compile-time dispatch of Max attributes to Fluid Parameters
/// We need set + get specialisations for each allowed type (Float,Long, Buffer, Enum, FloatArry, LongArray, BufferArray)
/// Note that set in the fluid base client *returns a function

// Setters

template<typename Client, size_t N, typename T, T Method(const t_atom *av)>
struct SetValue
{
  static void set(Client &x, t_object *attr, long ac, t_atom *av)
  {
    x.template setter<N>()(Method(av));
  }
};

template <typename Client, size_t N>
struct Setter<Client, FloatT, N> : public SetValue<Client, N, t_atom_float, &atom_getfloat> {};

template <typename Client, size_t N>
struct Setter<Client, LongT, N> : public SetValue<Client, N, t_atom_long, &atom_getlong> {};

template <typename Client, size_t N>
struct Setter<Client, EnumT, N> : public SetValue<Client, N, t_atom_long, &atom_getlong> {};

// Getters

template<typename Client, size_t N, typename T, t_max_err Method(t_atom *av, T)>
struct GetValue
{
  static void get(Client &x, t_object *attr, long *ac, t_atom **av)
  {
    char alloc;
    atom_alloc(ac, av, &alloc);
    (Method)(*av, x.template get<N>());
  }
};

template <typename Client, size_t N>
struct Getter<Client, FloatT, N> : public GetValue<Client, N, double, &atom_setfloat> {};

template <typename Client, size_t N>
struct Getter<Client, LongT, N> : public GetValue<Client, N, t_atom_long, &atom_setlong> {};

// Broken things
/*
 template <typename Client, size_t N>
 struct SetterDispatchImpl<Client, BufferT, N> {
 static void f(Client *x, t_object *attr, long ac, t_atom *av) {
 x->template setter<N>()(atom_getlong(av));
 }
 };
 
 template <typename Client, size_t N>
 struct SetterDispatchImpl<Client, FloatArrayT, N> {
 static void f(Client *x, t_object *attr, long ac, t_atom *av) {
 x->template setter<N>()(atom_getlong(av));
 }
 };
 
 template <typename Client, size_t N>
 struct SetterDispatchImpl<Client, LongArrayT, N> {
 static void f(Client *x, t_object *attr, long ac, t_atom *av) {
 x->template setter<N>()(atom_getlong(av));
 }
 };
 
 template <typename Client, size_t N>
 struct SetterDispatchImpl<Client, BufferArrayT, N> {
 static void f(Client *x, t_object *attr, long ac, t_atom *av) {
 x->template setter<N>()(atom_getlong(av));
 }
 };
 */
  
template<class Wrapper>
class RealTime
{
public:

  static void setup(t_class *c)
  {
    class_dspinit(c);
    class_addmethod(c, (method)callDSP, "dsp64", A_CANT, 0);
  }
  
  static void callDSP(Wrapper *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags) { x->dsp(dsp64, count, samplerate, maxvectorsize, flags); }
  
  static void callPerform(Wrapper *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long vec_size, long flags, void *userparam) {
      x->perform(dsp64, ins, numins, outs, numouts, vec_size, flags, userparam);
  }

  void dsp(t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags)
  {
    auto& client = static_cast<Wrapper*>(this)->mClient;
    Wrapper* wrapper = static_cast<Wrapper*>(this);

    audioInputConnections.resize(client.audioChannelsIn());
    std::copy(count,count + client.audioChannelsIn(),audioInputConnections.begin());
    
    audioOutputConnections.resize(client.audioChannelsOut());
    std::copy(count + client.audioChannelsIn(),count + client.audioChannelsIn() + client.audioChannelsOut(),audioOutputConnections.begin());
    mInputs.clear();
    mOutputs.clear();
    mInputs.reserve(client.audioChannelsIn());
    mOutputs.reserve(client.audioChannelsOut());
    std::fill_n(std::back_inserter(mInputs),client.audioChannelsIn(),FluidTensorView<double, 1>(nullptr,0,0));
    std::fill_n(std::back_inserter(mOutputs),client.audioChannelsOut(),FluidTensorView<double, 1>(nullptr,0,0));

    object_method(dsp64, gensym("dsp_add64"), wrapper, ((method) callPerform), 0, nullptr);
  }
  
  void perform(t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam)
  {
      auto& client = static_cast<Wrapper*>(this)->mClient;
      for(int i = 0; i < numins; ++i)
        if(audioInputConnections[i])
          mInputs[i].reset(ins[i],0,sampleframes);
  
      for(int i = 0; i < numouts; ++i)
        //if(audioOutputConnections[i])
          mOutputs[i].reset(outs[i],0,sampleframes);

     client.process(mInputs,mOutputs);
  }

private:
  std::vector<FluidTensorView<double,1>> mInputs;
  std::vector<FluidTensorView<double,1>> mOutputs;
  std::vector<short> audioInputConnections;
  std::vector<short> audioOutputConnections;
};
  
template <class Wrapper>
struct NonRealTime
{
  static void setup(t_class *c)
  {
    class_addmethod(c, (method)callProcess, "process", A_GIMME, 0);
  }
  
  void process(t_symbol* s, long ac, t_atom* av)
  {
    //Phase 1: Just take complete buffers as symbols
    std::vector<MaxBufferAdaptor> buffersIn;
    std::vector<MaxBufferAdaptor> buffersOut;
    
    std::vector<BufferProcessSpec> inputs;
    std::vector<BufferProcessSpec> outputs;
    
    auto& wrapper = static_cast<Wrapper&>(*this);
    auto& client = wrapper.mClient;

    if(ac != client.audioBuffersIn() + client.audioBuffersOut())
      object_error((t_object *)&wrapper, "Wrong number of buffers");
    
    buffersIn.reserve(client.audioBuffersIn());
    buffersOut.reserve(client.audioBuffersOut());
    
    for(int i = 0; i < client.audioBuffersIn(); ++i)
    {
      buffersIn.emplace_back(wrapper, atom_getsym(av + i));
      inputs.emplace_back();
      inputs[i].buffer = &buffersIn[i];
    }
    
    for (int i = client.audioBuffersIn(), j=0; i < client.audioBuffersIn() + client.audioBuffersOut(); ++i,++j)
    {
      buffersOut.emplace_back(wrapper, atom_getsym(av + i));
      outputs.emplace_back();
      outputs[j].buffer= &buffersOut[j];
    }
    
    client.process(inputs, outputs);
  }
  
  static void callProcess(Wrapper *x, t_symbol* s, long ac, t_atom* av)
  {
    x->process(s,ac,av);
  }
};

template <class Wrapper>
struct NonRealTimeAndRealTime : public RealTime<Wrapper>, public NonRealTime<Wrapper>
{
  static void setup(t_class *c)
  {
    RealTime<Wrapper>::setup(c);
    NonRealTime<Wrapper>::setup(c);
  }
};
  
// Base type selection
  
template <class Wrapper, typename NRT, typename RT>
struct FluidMaxBase { /* This shouldn't happen, but not sure how to throw an error if it does */ };

template<class Wrapper>
struct FluidMaxBase<Wrapper, std::true_type, std::false_type> : public NonRealTime<Wrapper> {};
  
template<class Wrapper>
struct FluidMaxBase<Wrapper, std::false_type, std::true_type> : public RealTime<Wrapper> {};
  
template<class Wrapper>
struct FluidMaxBase<Wrapper, std::true_type, std::true_type> : public NonRealTimeAndRealTime<Wrapper>  {};
  
} // namespace impl

template<typename T> using isRealTime = typename std::is_base_of<Audio,T>::type;
template<typename T> using isNonRealTime = typename std::is_base_of<Offline, T>::type;
  
template <typename Client>
class FluidMaxWrapper : public impl::FluidMaxBase<FluidMaxWrapper<Client>, isNonRealTime<Client>, isRealTime<Client>>
{
  friend impl::RealTime<FluidMaxWrapper<Client>>;
  friend impl::NonRealTime<FluidMaxWrapper<Client>>;
  
  using FluidMaxBase = impl::FluidMaxBase<FluidMaxWrapper<Client>, isNonRealTime<Client>, isRealTime<Client>>;
  
public:
  
  FluidMaxWrapper(t_symbol*, long ac, t_atom *av)
  {
    if (mClient.audioChannelsIn())
    {
      dsp_setup(&mObject, mClient.audioChannelsIn());
      // TODO - not sure if we need this assumption?? Let's assume not!
      //mObject.z_misc = Z_NO_INPLACE;
    }
    
    for (int i = 0; i < mClient.audioChannelsOut(); ++i)
      outlet_new(this, "signal");
  }
    
  FluidMaxWrapper(const FluidMaxWrapper&) = delete;
  FluidMaxWrapper& operator=(const FluidMaxWrapper&) = delete;

  static void *create(t_symbol *sym, long ac, t_atom *av)
  {
    // TODO - Check - I removed Owen's try/catch, because everything is bust if we run out of memory
    void *x = object_alloc(*getClass<Client>());
    new(x) FluidMaxWrapper(sym, ac, av);
    return x;
  }
  
  static void destroy(FluidMaxWrapper * x)
  {
    x->~FluidMaxWrapper();
  }
  
  static void makeClass(const char *className, typename Client::ParamType& params)
  {
    t_class** c = getClass<Client>();
    
    *c = class_new(className, (method)create, (method)destroy, sizeof(FluidMaxWrapper), 0, A_GIMME, 0);
    FluidMaxBase::setup(*c);
    class_register(CLASS_BOX, *c);
    processParameters(params, typename Client::ParamIndexList());
  }
  
private:
    
  template <class T>
  static t_class **getClass()
  {
    static t_class *C;
    return &C;
  }
  
  static t_symbol* maxAttrType(FloatT)  { return USESYM(float64);  }
  static t_symbol* maxAttrType(LongT)   { return USESYM(long);  }
  static t_symbol* maxAttrType(BufferT) { return USESYM(symbol);  }
  static t_symbol* maxAttrType(EnumT)   { return USESYM(long);  }
  
  // Sets up a single attribute
  // TODO: static assert on T?
  
  static std::string lowerCase(const char *str)
  {
    std::string result(str);
    std::transform(result.begin(),result.end(),result.begin(),[](unsigned char c){return std::tolower(c);});
    return result;
  }
    
  template <size_t N, typename T>
  static void setupAttribute(const T &attr)
  {
    using AttrType = std::remove_const_t<std::remove_reference_t<decltype(attr)>>;
    
    std::string name = lowerCase(attr.name);
    method setterMethod = (method) &impl::Setter<Client, AttrType, N>::set;
    method getterMethod = (method) &impl::Getter<Client, AttrType, N>::get;
    
    t_object *maxAttr = attribute_new(name.c_str(), maxAttrType(attr), 0, getterMethod, setterMethod);
    class_addattr(*getClass<FluidMaxWrapper>(), maxAttr);
  }
  
  ///Process the tuple of parameter descriptors
  
  template <size_t... Is>
  static void processParameters(typename Client::ParamType& params, std::index_sequence<Is...>)
  {
    (void)std::initializer_list<int>{ (setupAttribute<Is>(std::get<Is>(params).first), 0)...};
  }

  // The object structure
    
  t_pxobject mObject;
  Client mClient;
};

template <typename Client>
void makeMaxWrapper(const char *classname, typename Client::ParamType &params) {
  FluidMaxWrapper<Client>::makeClass(classname, params);
}

} // namespace client
} // namespace fluid
