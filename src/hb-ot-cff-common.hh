/*
 * Copyright © 2018 Adobe Systems Incorporated.
 *
 *  This is part of HarfBuzz, a text shaping library.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and its documentation for any purpose, provided that the
 * above copyright notice and the following two paragraphs appear in
 * all copies of this software.
 *
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
 * ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN
 * IF THE COPYRIGHT HOLDER HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * THE COPYRIGHT HOLDER SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE COPYRIGHT HOLDER HAS NO OBLIGATION TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 * Adobe Author(s): Michiharu Ariza
 */
#ifndef HB_OT_CFF_COMMON_HH
#define HB_OT_CFF_COMMON_HH

#include "hb-open-type.hh"
#include "hb-ot-layout-common.hh"
#include "hb-cff-interp-dict-common.hh"
#include "hb-subset-plan.hh"

namespace CFF {

using namespace OT;

/* utility macro */
template<typename Type>
static inline const Type& StructAtOffsetOrNull(const void *P, unsigned int offset)
{ return offset? (* reinterpret_cast<const Type*> ((const char *) P + offset)): Null(Type); }

inline unsigned int calcOffSize(unsigned int offset)
{
  unsigned int size = 1;
  while ((offset & ~0xFF) != 0)
  {
    size++;
    offset >>= 8;
  }
  assert (size <= 4);
  return size;
}

/* CFF INDEX */
template <typename COUNT>
struct CFFIndex
{
  inline bool sanitize (hb_sanitize_context_t *c) const
  {
    TRACE_SANITIZE (this);
    return_trace (likely ((count.sanitize (c) && count == 0) || /* empty INDEX */
                          (c->check_struct (this) && offSize >= 1 && offSize <= 4 &&
                           c->check_array (offsets, offSize, count + 1) &&
                           c->check_array (data_base (), 1, max_offset () - 1))));
  }

  inline static unsigned int calculate_offset_array_size (unsigned int offSize, unsigned int count)
  { return offSize * (count + 1); }

  inline unsigned int offset_array_size (void) const
  { return calculate_offset_array_size (offSize, count); }

  inline static unsigned int calculate_serialized_size (unsigned int offSize, unsigned int count, unsigned int dataSize)
  { return min_size + calculate_offset_array_size (offSize, count) + dataSize; }

  inline bool serialize (hb_serialize_context_t *c, const CFFIndex &src)
  {
    TRACE_SERIALIZE (this);
    unsigned int size = src.get_size ();
    CFFIndex *dest = c->allocate_size<CFFIndex> (size);
    if (unlikely (dest == nullptr)) return_trace (false);
    memcpy (dest, &src, size);
    return_trace (true);
  }

  inline bool serialize (hb_serialize_context_t *c,
                         unsigned int offSize_,
                         const hb_vector_t<ByteStr> &byteArray)
  {
    TRACE_SERIALIZE (this);
    /* serialize CFFIndex header */
    if (unlikely (!c->extend_min (*this))) return_trace (false);
    this->count.set (byteArray.len);
    this->offSize.set (offSize_);
    if (!unlikely (c->allocate_size<HBUINT8> (offSize_ * (byteArray.len + 1))))
      return_trace (false);
  
    /* serialize indices */
    unsigned int  offset = 1;
    unsigned int  i = 0;
    for (; i < byteArray.len; i++)
    {
      set_offset_at (i, offset);
      offset += byteArray[i].get_size ();
    }
    set_offset_at (i, offset);

    /* serialize data */
    for (unsigned int i = 0; i < byteArray.len; i++)
    {
      ByteStr  *dest = c->start_embed<ByteStr> ();
      if (unlikely (dest == nullptr ||
                    !dest->serialize (c, byteArray[i])))
        return_trace (false);
    }
    return_trace (true);
  }

  inline void set_offset_at (unsigned int index, unsigned int offset)
  {
    HBUINT8 *p = offsets + offSize * index + offSize;
    unsigned int size = offSize;
    for (; size; size--)
    {
      --p;
      p->set (offset & 0xFF);
      offset >>= 8;
    }
  }

  inline const unsigned int offset_at (unsigned int index) const
  {
    assert (index <= count);
    const HBUINT8 *p = offsets + offSize * index;
    unsigned int size = offSize;
    unsigned int offset = 0;
    for (; size; size--)
      offset = (offset << 8) + *p++;
    return offset;
  }

  inline const unsigned int length_at (unsigned int index) const
  { return offset_at (index + 1) - offset_at (index); }

  inline const char *data_base (void) const
  { return (const char *)this + min_size + offset_array_size (); }

  inline unsigned int data_size (void) const
  { return HBINT8::static_size; };

  ByteStr operator [] (unsigned int index) const
  {
    if (likely (index < count))
      return ByteStr (data_base () + offset_at (index) - 1, offset_at (index + 1) - offset_at (index));
    else
      return Null(ByteStr);
  }

  inline unsigned int get_size (void) const
  {
    if (this != &Null(CFFIndex))
    {
      if (count > 0)
        return min_size + offset_array_size () + (offset_at (count) - 1);
      else
        return count.static_size;  /* empty CFFIndex contains count only */
    }
    else
      return 0;
  }

  protected:
  inline unsigned int max_offset (void) const
  {
    unsigned int max = 0;
    for (unsigned int i = 0; i <= count; i++)
    {
      unsigned int off = offset_at (i);
      if (off > max) max = off;
    }
    return max;
  }

  public:
  COUNT     count;        /* Number of object data. Note there are (count+1) offsets */
  HBUINT8   offSize;      /* The byte size of each offset in the offsets array. */
  HBUINT8   offsets[VAR]; /* The array of (count + 1) offsets into objects array (1-base). */
  /* HBUINT8 data[VAR];      Object data */
  public:
  DEFINE_SIZE_ARRAY (COUNT::static_size + HBUINT8::static_size, offsets);
};

template <typename COUNT, typename TYPE>
struct CFFIndexOf : CFFIndex<COUNT>
{
  inline const ByteStr operator [] (unsigned int index) const
  {
    if (likely (index < CFFIndex<COUNT>::count))
      return ByteStr (CFFIndex<COUNT>::data_base () + CFFIndex<COUNT>::offset_at (index) - 1, CFFIndex<COUNT>::length_at (index));
    return Null(ByteStr);
  }

  template <typename DATA, typename PARAM1, typename PARAM2>
  inline bool serialize (hb_serialize_context_t *c,
                         unsigned int offSize_,
                         const DATA *dataArray,
                         unsigned int dataArrayLen,
                         const hb_vector_t<unsigned int> &dataSizeArray,
                         const PARAM1 &param1,
                         const PARAM2 &param2)
  {
    TRACE_SERIALIZE (this);
    /* serialize CFFIndex header */
    if (unlikely (!c->extend_min (*this))) return_trace (false);
    this->count.set (dataArrayLen);
    this->offSize.set (offSize_);
    if (!unlikely (c->allocate_size<HBUINT8> (offSize_ * (dataArrayLen + 1))))
      return_trace (false);
  
    /* serialize indices */
    unsigned int  offset = 1;
    unsigned int  i = 0;
    for (; i < dataArrayLen; i++)
    {
      CFFIndex<COUNT>::set_offset_at (i, offset);
      offset += dataSizeArray[i];
    }
    CFFIndex<COUNT>::set_offset_at (i, offset);

    /* serialize data */
    for (unsigned int i = 0; i < dataArrayLen; i++)
    {
      TYPE  *dest = c->start_embed<TYPE> ();
      if (unlikely (dest == nullptr ||
                    !dest->serialize (c, dataArray[i], param1, param2)))
        return_trace (false);
    }
    return_trace (true);
  }

  /* in parallel to above */
  template <typename DATA, typename PARAM>
  inline static unsigned int calculate_serialized_size (unsigned int &offSize_ /* OUT */,
                                                        const DATA *dataArray,
                                                        unsigned int dataArrayLen,
                                                        hb_vector_t<unsigned int> &dataSizeArray, /* OUT */
                                                        const PARAM &param)
  {
    /* determine offset size */
    unsigned int  totalDataSize = 0;
    for (unsigned int i = 0; i < dataArrayLen; i++)
    {
      unsigned int dataSize = TYPE::calculate_serialized_size (dataArray[i], param);
      dataSizeArray[i] = dataSize;
      totalDataSize += dataSize;
    }
    offSize_ = calcOffSize (totalDataSize);

    return CFFIndex<COUNT>::calculate_serialized_size (offSize_, dataArrayLen, totalDataSize);
  }
};

/* Top Dict, Font Dict, Private Dict */
struct Dict : UnsizedByteStr
{
  template <typename DICTVAL, typename OP_SERIALIZER, typename PARAM>
  inline bool serialize (hb_serialize_context_t *c,
                        const DICTVAL &dictval,
                        OP_SERIALIZER& opszr,
                        PARAM& param)
  {
    TRACE_SERIALIZE (this);
    for (unsigned int i = 0; i < dictval.getNumValues (); i++)
    {
      if (unlikely (!opszr.serialize (c, dictval[i], param)))
        return_trace (false);
    }
    return_trace (true);
  }

  /* in parallel to above */
  template <typename DICTVAL, typename OP_SERIALIZER>
  inline static unsigned int calculate_serialized_size (const DICTVAL &dictval,
                                                        OP_SERIALIZER& opszr)
  {
    unsigned int size = 0;
    for (unsigned int i = 0; i < dictval.getNumValues (); i++)
      size += opszr.calculate_serialized_size (dictval[i]);
    return size;
  }

  template <typename INTTYPE, int minVal, int maxVal>
  inline static bool serialize_offset_op (hb_serialize_context_t *c, OpCode op, int value, OpCode intOp)
  {
    if (value == 0)
      return true;
    // XXX: not sure why but LLVM fails to compile the following 'unlikely' macro invocation
    if (/*unlikely*/ (!serialize_int<INTTYPE, minVal, maxVal> (c, intOp, value)))
      return false;

    TRACE_SERIALIZE (this);
    /* serialize the opcode */
    HBUINT8 *p = c->allocate_size<HBUINT8> (OpCode_Size (op));
    if (unlikely (p == nullptr)) return_trace (false);
    if (Is_OpCode_ESC (op))
    {
      p->set (OpCode_escape);
      op = Unmake_OpCode_ESC (op);
      p++;
    }
    p->set (op);
    return_trace (true);
  }

  inline static bool serialize_offset4_op (hb_serialize_context_t *c, OpCode op, int value)
  { return serialize_offset_op<HBUINT32, 0, 0x7FFFFFFF> (c, op, value, OpCode_longintdict); }

  inline static bool serialize_offset2_op (hb_serialize_context_t *c, OpCode op, int value)
  { return serialize_offset_op<HBUINT16, 0, 0x7FFF> (c, op, value, OpCode_shortint); }
};

struct TopDict : Dict {};
struct FontDict : Dict {};
struct PrivateDict : Dict {};

struct TableInfo
{
  void init (void) { offSize = offset = size = 0; }

  unsigned int    offset;
  unsigned int    size;
  unsigned int    offSize;
};

/* font dict index remap table from fullset FDArray to subset FDArray.
 * set to HB_SET_VALUE_INVALID if excluded from subset */
struct FDMap : hb_vector_t<hb_codepoint_t>
{
  inline void init (void)
  { hb_vector_t<hb_codepoint_t>::init (); }

  inline void fini (void)
  { hb_vector_t<hb_codepoint_t>::fini (); }

  inline bool fullset (void) const
  {
    for (unsigned int i = 0; i < len; i++)
      if (hb_vector_t<hb_codepoint_t>::operator[] (i) == HB_SET_VALUE_INVALID)
        return false;
    return true;
  }

  inline bool excludes (hb_codepoint_t fd) const
  { return (fd < len) && ((*this)[fd] == HB_SET_VALUE_INVALID); }

  inline hb_codepoint_t operator[] (hb_codepoint_t i) const
  {
    if (fullset ())
      return i;
    else
      return hb_vector_t<hb_codepoint_t>::operator[] (i);
  }

  inline hb_codepoint_t &operator[] (hb_codepoint_t i)
  {
    assert (i < len);
    return hb_vector_t<hb_codepoint_t>::operator[] (i);
  }
};

template <typename COUNT>
struct FDArray : CFFIndexOf<COUNT, FontDict>
{
  template <typename DICTVAL, typename OP_SERIALIZER>
  inline bool serialize (hb_serialize_context_t *c,
                        unsigned int offSize_,
                        const hb_vector_t<DICTVAL> &fontDicts,
                        unsigned int fdCount,
                        const FDMap &fdmap,
                        OP_SERIALIZER& opszr,
                        const hb_vector_t<TableInfo> &privateInfos)
  {
    TRACE_SERIALIZE (this);
    if (unlikely (!c->extend_min (*this))) return_trace (false);
    this->count.set (fdCount);
    this->offSize.set (offSize_);
    if (!unlikely (c->allocate_size<HBUINT8> (offSize_ * (fdCount + 1))))
      return_trace (false);
    
    /* serialize font dict offsets */
    unsigned int  offset = 1;
    unsigned int  fid = 0;
    for (unsigned i = 0; i < fontDicts.len; i++)
      if (!fdmap.excludes (i))
      {
        CFFIndexOf<COUNT, FontDict>::set_offset_at (fid++, offset);
        offset += FontDict::calculate_serialized_size (fontDicts[i], opszr);
      }
    CFFIndexOf<COUNT, FontDict>::set_offset_at (fid, offset);

    /* serialize font dicts */
    for (unsigned int i = 0; i < fontDicts.len; i++)
      if (!fdmap.excludes (i))
      {
        FontDict *dict = c->start_embed<FontDict> ();
        if (unlikely (!dict->serialize (c, fontDicts[i], opszr, privateInfos[fdmap[i]])))
          return_trace (false);
      }
    return_trace (true);
  }
  
  /* in parallel to above */
  template <typename OP_SERIALIZER, typename DICTVAL>
  inline static unsigned int calculate_serialized_size (unsigned int &offSize_ /* OUT */,
                                                        const hb_vector_t<DICTVAL> &fontDicts,
                                                        unsigned int fdCount,
                                                        const FDMap &fdmap,
                                                        OP_SERIALIZER& opszr)
  {
    unsigned int dictsSize = 0;
    for (unsigned int i = 0; i < fontDicts.len; i++)
      if (!fdmap.excludes (i))
        dictsSize += FontDict::calculate_serialized_size (fontDicts[i], opszr);

    offSize_ = calcOffSize (dictsSize + 1);
    return CFFIndex<COUNT>::calculate_serialized_size (offSize_, fdCount, dictsSize);
  }
};

/* FDSelect */
struct FDSelect0 {
  inline bool sanitize (hb_sanitize_context_t *c, unsigned int fdcount) const
  {
    TRACE_SANITIZE (this);
    if (unlikely (!(c->check_struct (this))))
      return_trace (false);
    for (unsigned int i = 0; i < c->get_num_glyphs (); i++)
      if (unlikely (!fds[i].sanitize (c)))
        return_trace (false);

    return_trace (true);
  }

  inline hb_codepoint_t get_fd (hb_codepoint_t glyph) const
  {
    return (hb_codepoint_t)fds[glyph];
  }

  inline unsigned int get_size (unsigned int num_glyphs) const
  { return HBUINT8::static_size * num_glyphs; }

  HBUINT8     fds[VAR];

  DEFINE_SIZE_MIN (1);
};

template <typename GID_TYPE, typename FD_TYPE>
struct FDSelect3_4_Range {
  inline bool sanitize (hb_sanitize_context_t *c, unsigned int fdcount) const
  {
    TRACE_SANITIZE (this);
    return_trace (likely (c->check_struct (this) && (first < c->get_num_glyphs ()) && (fd < fdcount)));
  }

  GID_TYPE    first;
  FD_TYPE     fd;

  DEFINE_SIZE_STATIC (GID_TYPE::static_size + FD_TYPE::static_size);
};

template <typename GID_TYPE, typename FD_TYPE>
struct FDSelect3_4 {
  inline unsigned int get_size (void) const
  { return GID_TYPE::static_size * 2 + FDSelect3_4_Range<GID_TYPE, FD_TYPE>::static_size * nRanges; }

  inline bool sanitize (hb_sanitize_context_t *c, unsigned int fdcount) const
  {
    TRACE_SANITIZE (this);
    if (unlikely (!(c->check_struct (this) && (nRanges > 0) && (ranges[0].first == 0))))
      return_trace (false);

    for (unsigned int i = 0; i < nRanges; i++)
    {
      if (unlikely (!ranges[i].sanitize (c, fdcount)))
        return_trace (false);
      if ((0 < i) && unlikely (ranges[i - 1].first >= ranges[i].first))
        return_trace (false);
    }
    if (unlikely (!sentinel().sanitize (c) || (sentinel() != c->get_num_glyphs ())))
      return_trace (false);

    return_trace (true);
  }

  inline hb_codepoint_t get_fd (hb_codepoint_t glyph) const
  {
    for (unsigned int i = 1; i < nRanges; i++)
      if (glyph < ranges[i].first)
        return (hb_codepoint_t)ranges[i - 1].fd;

    assert (false);
  }

  inline GID_TYPE &sentinel (void)  { return StructAfter<GID_TYPE> (ranges[nRanges - 1]); }
  inline const GID_TYPE &sentinel (void) const  { return StructAfter<GID_TYPE> (ranges[nRanges - 1]); }

  GID_TYPE         nRanges;
  FDSelect3_4_Range<GID_TYPE, FD_TYPE>  ranges[VAR];
  /* GID_TYPE sentinel */

  DEFINE_SIZE_ARRAY (GID_TYPE::static_size * 2, ranges);
};

typedef FDSelect3_4<HBUINT16, HBUINT8> FDSelect3;
typedef FDSelect3_4_Range<HBUINT16, HBUINT8> FDSelect3_Range;

struct FDSelect {
  inline bool sanitize (hb_sanitize_context_t *c, unsigned int fdcount) const
  {
    TRACE_SANITIZE (this);

    return_trace (likely (c->check_struct (this) && (format == 0 || format == 3) &&
                          (format == 0)?
                          u.format0.sanitize (c, fdcount):
                          u.format3.sanitize (c, fdcount)));
  }

  inline bool serialize (hb_serialize_context_t *c, const FDSelect &src, unsigned int num_glyphs)
  {
    TRACE_SERIALIZE (this);
    unsigned int size = src.get_size (num_glyphs);
    FDSelect *dest = c->allocate_size<FDSelect> (size);
    if (unlikely (dest == nullptr)) return_trace (false);
    memcpy (dest, &src, size);
    return_trace (true);
  }

  inline unsigned int calculate_serialized_size (unsigned int num_glyphs) const
  { return get_size (num_glyphs); }

  inline unsigned int get_size (unsigned int num_glyphs) const
  {
    unsigned int size = format.static_size;
    if (format == 0)
      size += u.format0.get_size (num_glyphs);
    else
      size += u.format3.get_size ();
    return size;
  }

  inline hb_codepoint_t get_fd (hb_codepoint_t glyph) const
  {
    if (this == &Null(FDSelect))
      return 0;
    if (format == 0)
      return u.format0.get_fd (glyph);
    else
      return u.format3.get_fd (glyph);
  }

  HBUINT8       format;
  union {
    FDSelect0   format0;
    FDSelect3   format3;
  } u;

  DEFINE_SIZE_MIN (1);
};

template <typename COUNT>
struct Subrs : CFFIndex<COUNT>
{
  inline bool serialize (hb_serialize_context_t *c, const Subrs<COUNT> &subrs, unsigned int offSize, const hb_set_t *set, const ByteStr& nullStr = ByteStr())
  {
    TRACE_SERIALIZE (this);
    if (&subrs == &Null(Subrs<COUNT>))
      return_trace (true);
    if ((subrs.count == 0) || (set == nullptr) || (hb_set_is_empty (set)))
    {
      if (!unlikely (c->allocate_size<COUNT> (COUNT::static_size)))
        return_trace (false);
      CFFIndex<COUNT>::count.set (0);
      return_trace (true);
    }
    
    hb_vector_t<ByteStr> bytesArray;
    bytesArray.init ();
    if (!bytesArray.resize (subrs.count))
      return_trace (false);
    for (hb_codepoint_t i = 0; i < subrs.count; i++)
      bytesArray[i] = (hb_set_has (set, i))? subrs[i]: nullStr;

    bool result = CFFIndex<COUNT>::serialize (c, offSize, bytesArray);
    bytesArray.fini ();
    return_trace (result);
  }
  
  /* in parallel to above */
  inline unsigned int calculate_serialized_size (unsigned int &offSize /*OUT*/, const hb_set_t *set, unsigned int nullStrSize = 0) const
  {
    if (this == &Null(Subrs<COUNT>))
      return 0;
    unsigned int  count_ = CFFIndex<COUNT>::count;
    offSize = 0;
    if ((count_ == 0) || (hb_set_get_population (set) == 0))
      return COUNT::static_size;

    unsigned int dataSize = 0;
    for (hb_codepoint_t i = 0; i < count_; i++)
    {
      if (hb_set_has (set, i))
        dataSize += (*this)[i].len;
      else
        dataSize += nullStrSize;
    }
    offSize = calcOffSize(dataSize);
    return CFFIndex<COUNT>::calculate_serialized_size (offSize, count_, dataSize);
  }
};

} /* namespace CFF */

#endif /* HB_OT_CFF_COMMON_HH */

