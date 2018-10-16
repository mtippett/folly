#pragma once
// Copyright (c) 2018-present, Facebook, Inc.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "single.h"
#include "executor.h"
#include "inline.h"

namespace pushmi {

template <class V, class E, class CV>
class any_constrained_single_sender {
  union data {
    void* pobj_ = nullptr;
    char buffer_[sizeof(std::promise<int>)]; // can hold a V in-situ
  } data_{};
  template <class Wrapped>
  static constexpr bool insitu() {
    return sizeof(Wrapped) <= sizeof(data::buffer_) &&
        std::is_nothrow_move_constructible<Wrapped>::value;
  }
  struct vtable {
    static void s_op(data&, data*) {}
    static CV s_top(data&) { return CV{}; }
    static any_constrained_executor<E, CV> s_executor(data&) { return {}; }
    static void s_submit(data&, CV, single<V, E>) {}
    void (*op_)(data&, data*) = vtable::s_op;
    CV (*top_)(data&) = vtable::s_top;
    any_constrained_executor<E, CV> (*executor_)(data&) = vtable::s_executor;
    void (*submit_)(data&, CV, single<V, E>) = vtable::s_submit;
  };
  static constexpr vtable const noop_ {};
  vtable const* vptr_ = &noop_;
  template <class Wrapped>
  any_constrained_single_sender(Wrapped obj, std::false_type)
    : any_constrained_single_sender() {
    struct s {
      static void op(data& src, data* dst) {
        if (dst)
          dst->pobj_ = std::exchange(src.pobj_, nullptr);
        delete static_cast<Wrapped const*>(src.pobj_);
      }
      static CV top(data& src) {
        return ::pushmi::top(*static_cast<Wrapped*>(src.pobj_));
      }
      static any_constrained_executor<E, CV> executor(data& src) {
        return any_constrained_executor<E, CV>{::pushmi::executor(*static_cast<Wrapped*>(src.pobj_))};
      }
      static void submit(data& src, CV at, single<V, E> out) {
        ::pushmi::submit(
            *static_cast<Wrapped*>(src.pobj_),
            std::move(at),
            std::move(out));
      }
    };
    static const vtable vtbl{s::op, s::top, s::executor, s::submit};
    data_.pobj_ = new Wrapped(std::move(obj));
    vptr_ = &vtbl;
  }
  template <class Wrapped>
  any_constrained_single_sender(Wrapped obj, std::true_type) noexcept
    : any_constrained_single_sender() {
    struct s {
      static void op(data& src, data* dst) {
        if (dst)
          new (dst->buffer_) Wrapped(
              std::move(*static_cast<Wrapped*>((void*)src.buffer_)));
        static_cast<Wrapped const*>((void*)src.buffer_)->~Wrapped();
      }
      static CV top(data& src) {
        return ::pushmi::top(*static_cast<Wrapped*>((void*)src.buffer_));
      }
      static any_constrained_executor<E, CV> executor(data& src) {
        return any_constrained_executor<E, CV>{::pushmi::executor(*static_cast<Wrapped*>((void*)src.buffer_))};
      }
      static void submit(data& src, CV cv, single<V, E> out) {
        ::pushmi::submit(
            *static_cast<Wrapped*>((void*)src.buffer_),
            std::move(cv),
            std::move(out));
      }
    };
    static const vtable vtbl{s::op, s::top, s::executor, s::submit};
    new (data_.buffer_) Wrapped(std::move(obj));
    vptr_ = &vtbl;
  }
  template <class T, class U = std::decay_t<T>>
  using wrapped_t =
    std::enable_if_t<!std::is_same<U, any_constrained_single_sender>::value, U>;

 public:
  using properties = property_set<is_constrained<>, is_single<>>;

  any_constrained_single_sender() = default;
  any_constrained_single_sender(any_constrained_single_sender&& that) noexcept
      : any_constrained_single_sender() {
    that.vptr_->op_(that.data_, &data_);
    std::swap(that.vptr_, vptr_);
  }
  PUSHMI_TEMPLATE (class Wrapped)
    (requires ConstrainedSenderTo<wrapped_t<Wrapped>, single<V, E>>)
  explicit any_constrained_single_sender(Wrapped obj) noexcept(insitu<Wrapped>())
  : any_constrained_single_sender{std::move(obj), bool_<insitu<Wrapped>()>{}} {
  }
  ~any_constrained_single_sender() {
    vptr_->op_(data_, nullptr);
  }
  any_constrained_single_sender& operator=(any_constrained_single_sender&& that) noexcept {
    this->~any_constrained_single_sender();
    new ((void*)this) any_constrained_single_sender(std::move(that));
    return *this;
  }
  CV top() {
    return vptr_->top_(data_);
  }
  any_constrained_executor<E, CV> executor() {
    return vptr_->executor_(data_);
  }
  void submit(CV at, single<V, E> out) {
    vptr_->submit_(data_, std::move(at), std::move(out));
  }
};

// Class static definitions:
template <class V, class E, class CV>
constexpr typename any_constrained_single_sender<V, E, CV>::vtable const
    any_constrained_single_sender<V, E, CV>::noop_;

template<class SF, class ZF, class EXF>
  // (requires Invocable<ZF&> && Invocable<EXF&> PUSHMI_BROKEN_SUBSUMPTION(&& not Sender<SF>))
class constrained_single_sender<SF, ZF, EXF> {
  SF sf_;
  EXF exf_;
  ZF zf_;

 public:
  using properties = property_set<is_constrained<>, is_single<>>;

  constexpr constrained_single_sender() = default;
  constexpr explicit constrained_single_sender(SF sf)
      : sf_(std::move(sf)) {}
  constexpr constrained_single_sender(SF sf, EXF exf)
      : sf_(std::move(sf)), exf_(std::move(exf)) {}
  constexpr constrained_single_sender(SF sf, EXF exf, ZF zf)
      : sf_(std::move(sf)), zf_(std::move(zf)), exf_(std::move(exf)) {}

  auto top() {
    return zf_();
  }
  auto executor() { return exf_(); }
  PUSHMI_TEMPLATE(class CV, class Out)
    (requires Regular<CV> && Receiver<Out, is_single<>> &&
      Invocable<SF&, CV, Out>)
  void submit(CV cv, Out out) {
    sf_(std::move(cv), std::move(out));
  }
};

template <PUSHMI_TYPE_CONSTRAINT(ConstrainedSender<is_single<>>) Data, class DSF, class DZF, class DEXF>
#if __cpp_concepts
  requires Invocable<DZF&, Data&> && Invocable<DEXF&, Data&>
#endif
class constrained_single_sender<Data, DSF, DZF, DEXF> {
  Data data_;
  DSF sf_;
  DEXF exf_;
  DZF zf_;

 public:
  using properties = property_set_insert_t<properties_t<Data>, property_set<is_single<>>>;

  constexpr constrained_single_sender() = default;
  constexpr explicit constrained_single_sender(Data data)
      : data_(std::move(data)) {}
  constexpr constrained_single_sender(Data data, DSF sf, DEXF exf = DEXF{})
      : data_(std::move(data)), sf_(std::move(sf)), exf_(std::move(exf)) {}
  constexpr constrained_single_sender(Data data, DSF sf, DEXF exf, DZF zf)
      : data_(std::move(data)), sf_(std::move(sf)), zf_(std::move(zf)), exf_(std::move(exf)) {}

  auto top() {
    return zf_(data_);
  }
  auto executor() { return exf_(data_); }
  PUSHMI_TEMPLATE(class CV, class Out)
    (requires Regular<CV> && Receiver<Out, is_single<>> &&
      Invocable<DSF&, Data&, CV, Out>)
  void submit(CV cv, Out out) {
    sf_(data_, std::move(cv), std::move(out));
  }
};

template <>
class constrained_single_sender<>
    : public constrained_single_sender<ignoreSF, priorityZeroF, inlineConstrainedEXF> {
public:
  constrained_single_sender() = default;
};

////////////////////////////////////////////////////////////////////////////////
// make_constrained_single_sender
PUSHMI_INLINE_VAR constexpr struct make_constrained_single_sender_fn {
  inline auto operator()() const  {
    return constrained_single_sender<ignoreSF, priorityZeroF, inlineConstrainedEXF>{};
  }
  PUSHMI_TEMPLATE(class SF)
    (requires True<> PUSHMI_BROKEN_SUBSUMPTION(&& not Sender<SF>))
  auto operator()(SF sf) const {
    return constrained_single_sender<SF, priorityZeroF, inlineConstrainedEXF>{std::move(sf)};
  }
  PUSHMI_TEMPLATE (class SF, class EXF)
    (requires Invocable<EXF&> PUSHMI_BROKEN_SUBSUMPTION(&& not Sender<SF>))
  auto operator()(SF sf, EXF exf) const {
    return constrained_single_sender<SF, priorityZeroF, EXF>{std::move(sf), std::move(exf)};
  }
  PUSHMI_TEMPLATE (class SF, class ZF, class EXF)
    (requires Invocable<ZF&> && Invocable<EXF&> PUSHMI_BROKEN_SUBSUMPTION(&& not Sender<SF>))
  auto operator()(SF sf, EXF exf, ZF zf) const {
    return constrained_single_sender<SF, ZF, EXF>{std::move(sf), std::move(exf), std::move(zf)};
  }
  PUSHMI_TEMPLATE (class Data)
    (requires ConstrainedSender<Data, is_single<>>)
  auto operator()(Data d) const {
    return constrained_single_sender<Data, passDSF, passDZF, passDEXF>{std::move(d)};
  }
  PUSHMI_TEMPLATE (class Data, class DSF)
    (requires ConstrainedSender<Data, is_single<>>)
  auto operator()(Data d, DSF sf) const {
    return constrained_single_sender<Data, DSF, passDZF, passDEXF>{std::move(d), std::move(sf)};
  }
  PUSHMI_TEMPLATE (class Data, class DSF, class DEXF)
    (requires ConstrainedSender<Data, is_single<>> && Invocable<DEXF&, Data&>)
  auto operator()(Data d, DSF sf, DEXF exf) const  {
    return constrained_single_sender<Data, DSF, passDZF, DEXF>{std::move(d), std::move(sf),
      std::move(exf)};
  }
  PUSHMI_TEMPLATE (class Data, class DSF, class DZF, class DEXF)
    (requires ConstrainedSender<Data, is_single<>> && Invocable<DZF&, Data&> && Invocable<DEXF&, Data&>)
  auto operator()(Data d, DSF sf, DEXF exf, DZF zf) const  {
    return constrained_single_sender<Data, DSF, DZF, DEXF>{std::move(d), std::move(sf),
      std::move(exf), std::move(zf)};
  }
} const make_constrained_single_sender {};

////////////////////////////////////////////////////////////////////////////////
// deduction guides
#if __cpp_deduction_guides >= 201703
constrained_single_sender() -> constrained_single_sender<ignoreSF, priorityZeroF, inlineConstrainedEXF>;

PUSHMI_TEMPLATE(class SF)
  (requires True<> PUSHMI_BROKEN_SUBSUMPTION(&& not Sender<SF>))
constrained_single_sender(SF) -> constrained_single_sender<SF, priorityZeroF, inlineConstrainedEXF>;

PUSHMI_TEMPLATE (class SF, class EXF)
  (requires Invocable<EXF&> PUSHMI_BROKEN_SUBSUMPTION(&& not Sender<SF>))
constrained_single_sender(SF, EXF) -> constrained_single_sender<SF, priorityZeroF, EXF>;

PUSHMI_TEMPLATE (class SF, class ZF, class EXF)
  (requires Invocable<ZF&> && Invocable<EXF&> PUSHMI_BROKEN_SUBSUMPTION(&& not Sender<SF>))
constrained_single_sender(SF, EXF, ZF) -> constrained_single_sender<SF, ZF, EXF>;

PUSHMI_TEMPLATE (class Data, class DSF)
  (requires ConstrainedSender<Data, is_single<>>)
constrained_single_sender(Data, DSF) -> constrained_single_sender<Data, DSF, passDZF, passDEXF>;

PUSHMI_TEMPLATE (class Data, class DSF, class DEXF)
  (requires ConstrainedSender<Data, is_single<>> && Invocable<DEXF&, Data&>)
constrained_single_sender(Data, DSF, DEXF) -> constrained_single_sender<Data, DSF, passDZF, DEXF>;

PUSHMI_TEMPLATE (class Data, class DSF, class DZF, class DEXF)
  (requires ConstrainedSender<Data, is_single<>> && Invocable<DZF&, Data&> && Invocable<DEXF&, Data&>)
constrained_single_sender(Data, DSF, DEXF, DZF) -> constrained_single_sender<Data, DSF, DZF, DEXF>;
#endif

template<>
struct construct_deduced<constrained_single_sender>
  : make_constrained_single_sender_fn {};

// template <
//     class V,
//     class E = std::exception_ptr,
//     class CV = std::chrono::system_clock::time_point,
//     ConstrainedSenderTo<single<V, E>, is_single<>> Wrapped>
// auto erase_cast(Wrapped w) {
//   return constrained_single_sender<V, E>{std::move(w)};
// }

} // namespace pushmi