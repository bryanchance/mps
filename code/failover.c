/* failover.c: FAILOVER IMPLEMENTATION
 *
 * $Id$
 * Copyright (c) 2014 Ravenbrook Limited.  See end of file for license.
 *
 * .design: <design/failover>
 *
 * .critical: In manual-allocation-bound programs using MVFF, many of
 * these functions are on the critical paths via mps_alloc (and then
 * PoolAlloc, MVFFAlloc, failoverFind*) and mps_free (and then
 * MVFFFree, failoverInsert).
 */

#include "failover.h"
#include "mpm.h"
#include "range.h"

SRCID(failover, "$Id$");


ARG_DEFINE_KEY(failover_primary, Pointer);
ARG_DEFINE_KEY(failover_secondary, Pointer);


Bool FailoverCheck(Failover fo)
{
  CHECKS(Failover, fo);
  CHECKD(Land, &fo->landStruct);
  CHECKD(Land, fo->primary);
  CHECKD(Land, fo->secondary);
  return TRUE;
}


static Res failoverInit(Land land, Arena arena, Align alignment, ArgList args)
{
  Failover fo;
  ArgStruct arg;
  Res res;

  AVER(land != NULL);
  res = NextMethod(Land, Failover, init)(land, arena, alignment, args);
  if (res != ResOK)
    return res;
  fo = CouldBeA(Failover, land);

  ArgRequire(&arg, args, FailoverPrimary);
  fo->primary = arg.val.p;
  ArgRequire(&arg, args, FailoverSecondary);
  fo->secondary = arg.val.p;

  SetClassOfPoly(land, CLASS(Failover));
  fo->sig = FailoverSig;
  AVERC(Failover, fo);

  return ResOK;
}


static void failoverFinish(Inst inst)
{
  Land land = MustBeA(Land, inst);
  Failover fo = MustBeA(Failover, land);
  fo->sig = SigInvalid;
  NextMethod(Inst, Failover, finish)(inst);
}


static Size failoverSize(Land land)
{
  Failover fo = MustBeA_CRITICAL(Failover, land);
  return LandSize(fo->primary) + LandSize(fo->secondary);
}


static Res failoverInsert(Range rangeReturn, Land land, Range range)
{
  Failover fo = MustBeA_CRITICAL(Failover, land);
  Res res;

  AVER_CRITICAL(rangeReturn != NULL);
  AVERT_CRITICAL(Range, range);

  /* Provide more opportunities for coalescence. See
   * <design/failover#.impl.assume.flush>.
   */
  (void)LandFlush(fo->primary, fo->secondary);

  res = LandInsert(rangeReturn, fo->primary, range);
  if (res != ResOK && res != ResFAIL)
    res = LandInsert(rangeReturn, fo->secondary, range);

  return res;
}


static Res failoverInsertSteal(Range rangeReturn, Land land, Range rangeIO)
{
  Failover fo = MustBeA(Failover, land);
  Res res;

  AVER(rangeReturn != NULL);
  AVER(rangeReturn != rangeIO);
  AVERT(Range, rangeIO);

  /* Provide more opportunities for coalescence. See
   * <design/failover#.impl.assume.flush>.
   */
  (void)LandFlush(fo->primary, fo->secondary);

  res = LandInsertSteal(rangeReturn, fo->primary, rangeIO);
  AVER(res == ResOK || res == ResFAIL);
  return res;
}


static Res failoverDelete(Range rangeReturn, Land land, Range range)
{
  Failover fo = MustBeA(Failover, land);
  Res res;
  RangeStruct oldRange, dummyRange, left, right;

  AVER(rangeReturn != NULL);
  AVERT(Range, range);

  /* Prefer efficient search in the primary. See
   * <design/failover#.impl.assume.flush>.
   */
  (void)LandFlush(fo->primary, fo->secondary);

  res = LandDelete(&oldRange, fo->primary, range);

  if (res == ResFAIL) {
    /* Range not found in primary: try secondary. */
    return LandDelete(rangeReturn, fo->secondary, range);
  } else if (res != ResOK) {
    /* Range was found in primary, but couldn't be deleted. The only
     * case we expect to encounter here is the case where the primary
     * is out of memory. (In particular, we don't handle the case of a
     * CBS returning ResLIMIT because its block pool has been
     * configured not to automatically extend itself.)
     */
    AVER(ResIsAllocFailure(res));

    /* Delete the whole of oldRange, and re-insert the fragments
     * (which might end up in the secondary). See
     * <design/failover#.impl.assume.delete>.
     */
    res = LandDelete(&dummyRange, fo->primary, &oldRange);
    if (res != ResOK)
      return res;

    AVER(RangesEqual(&oldRange, &dummyRange));
    RangeInit(&left, RangeBase(&oldRange), RangeBase(range));
    if (!RangeIsEmpty(&left)) {
      /* Don't call LandInsert(..., land, ...) here: that would be
       * re-entrant and fail the landEnter check. */
      res = LandInsert(&dummyRange, fo->primary, &left);
      if (res != ResOK) {
        /* The range was successful deleted from the primary above. */
        AVER(res != ResFAIL);
        res = LandInsert(&dummyRange, fo->secondary, &left);
        AVER(res == ResOK);
      }
    }
    RangeInit(&right, RangeLimit(range), RangeLimit(&oldRange));
    if (!RangeIsEmpty(&right)) {
      res = LandInsert(&dummyRange, fo->primary, &right);
      if (res != ResOK) {
        /* The range was successful deleted from the primary above. */
        AVER(res != ResFAIL);
        res = LandInsert(&dummyRange, fo->secondary, &right);
        AVER(res == ResOK);
      }
    }
  }
  if (res == ResOK) {
    AVER_CRITICAL(RangesNest(&oldRange, range));
    RangeCopy(rangeReturn, &oldRange);
  }
  return res;
}


static Res failoverDeleteSteal(Range rangeReturn, Land land, Range range)
{
  Failover fo = MustBeA(Failover, land);
  Res res;

  AVER(rangeReturn != NULL);
  AVERT(Range, range);

  /* Prefer efficient search in the primary. See
   * <design/failover#.impl.assume.flush>.
   */
  (void)LandFlush(fo->primary, fo->secondary);

  res = LandDeleteSteal(rangeReturn, fo->primary, range);
  if (res == ResFAIL)
    /* Not found in primary: try secondary. */
    res = LandDeleteSteal(rangeReturn, fo->secondary, range);
  AVER(res == ResOK || res == ResFAIL);
  return res;
}


static Bool failoverIterate(Land land, LandVisitor visitor, void *closure)
{
  Failover fo = MustBeA(Failover, land);

  AVER(visitor != NULL);

  return LandIterate(fo->primary, visitor, closure)
    && LandIterate(fo->secondary, visitor, closure);
}


static Bool failoverFindFirst(Range rangeReturn, Range oldRangeReturn, Land land, Size size, FindDelete findDelete)
{
  Failover fo = MustBeA_CRITICAL(Failover, land);

  AVER_CRITICAL(rangeReturn != NULL);
  AVER_CRITICAL(oldRangeReturn != NULL);
  AVERT_CRITICAL(FindDelete, findDelete);

  /* <design/failover#.impl.assume.flush>. */
  (void)LandFlush(fo->primary, fo->secondary);

  return LandFindFirst(rangeReturn, oldRangeReturn, fo->primary, size, findDelete)
    || LandFindFirst(rangeReturn, oldRangeReturn, fo->secondary, size, findDelete);
}


static Bool failoverFindLast(Range rangeReturn, Range oldRangeReturn, Land land, Size size, FindDelete findDelete)
{
  Failover fo = MustBeA_CRITICAL(Failover, land);

  AVER_CRITICAL(rangeReturn != NULL);
  AVER_CRITICAL(oldRangeReturn != NULL);
  AVERT_CRITICAL(FindDelete, findDelete);

  /* <design/failover#.impl.assume.flush>. */
  (void)LandFlush(fo->primary, fo->secondary);

  return LandFindLast(rangeReturn, oldRangeReturn, fo->primary, size, findDelete)
    || LandFindLast(rangeReturn, oldRangeReturn, fo->secondary, size, findDelete);
}


static Bool failoverFindLargest(Range rangeReturn, Range oldRangeReturn, Land land, Size size, FindDelete findDelete)
{
  Failover fo = MustBeA_CRITICAL(Failover, land);

  AVER_CRITICAL(rangeReturn != NULL);
  AVER_CRITICAL(oldRangeReturn != NULL);
  AVERT_CRITICAL(FindDelete, findDelete);

  /* <design/failover#.impl.assume.flush>. */
  (void)LandFlush(fo->primary, fo->secondary);

  return LandFindLargest(rangeReturn, oldRangeReturn, fo->primary, size, findDelete)
    || LandFindLargest(rangeReturn, oldRangeReturn, fo->secondary, size, findDelete);
}


static Bool failoverFindInZones(Bool *foundReturn, Range rangeReturn, Range oldRangeReturn, Land land, Size size, ZoneSet zoneSet, Bool high)
{
  Failover fo = MustBeA_CRITICAL(Failover, land);
  Bool found = FALSE;
  Res res;

  AVER_CRITICAL(FALSE); /* TODO: this code is completely untested! */
  AVER_CRITICAL(foundReturn != NULL);
  AVER_CRITICAL(rangeReturn != NULL);
  AVER_CRITICAL(oldRangeReturn != NULL);
  /* AVERT_CRITICAL(ZoneSet, zoneSet); */
  AVERT_CRITICAL(Bool, high);

  /* <design/failover#.impl.assume.flush>. */
  (void)LandFlush(fo->primary, fo->secondary);

  res = LandFindInZones(&found, rangeReturn, oldRangeReturn, fo->primary, size, zoneSet, high);
  if (res != ResOK || !found)
    res = LandFindInZones(&found, rangeReturn, oldRangeReturn, fo->secondary, size, zoneSet, high);

  *foundReturn = found;
  return res;
}


static Res failoverDescribe(Inst inst, mps_lib_FILE *stream, Count depth)
{
  Land land = CouldBeA(Land, inst);
  Failover fo = CouldBeA(Failover, land);
  LandClass primaryClass, secondaryClass;
  Res res;

  if (!TESTC(Failover, fo))
    return ResPARAM;
  if (stream == NULL)
    return ResPARAM;

  res = NextMethod(Inst, Failover, describe)(inst, stream, depth);
  if (res != ResOK)
    return res;

  primaryClass = ClassOfPoly(Land, fo->primary);
  secondaryClass = ClassOfPoly(Land, fo->secondary);

  return WriteF(stream, depth + 2,
                "primary = $P ($S)\n",
                (WriteFP)fo->primary,
                (WriteFS)ClassName(primaryClass),
                "secondary = $P ($S)\n",
                (WriteFP)fo->secondary,
                (WriteFS)ClassName(secondaryClass),
                NULL);
}


DEFINE_CLASS(Land, Failover, klass)
{
  INHERIT_CLASS(klass, Failover, Land);
  klass->instClassStruct.describe = failoverDescribe;
  klass->instClassStruct.finish = failoverFinish;
  klass->size = sizeof(FailoverStruct);
  klass->init = failoverInit;
  klass->sizeMethod = failoverSize;
  klass->insert = failoverInsert;
  klass->insertSteal = failoverInsertSteal;
  klass->delete = failoverDelete;
  klass->deleteSteal = failoverDeleteSteal;
  klass->iterate = failoverIterate;
  klass->findFirst = failoverFindFirst;
  klass->findLast = failoverFindLast;
  klass->findLargest = failoverFindLargest;
  klass->findInZones = failoverFindInZones;
  AVERT(LandClass, klass);
}


/* C. COPYRIGHT AND LICENSE
 *
 * Copyright (C) 2014 Ravenbrook Limited <http://www.ravenbrook.com/>.
 * All rights reserved.  This is an open source license.  Contact
 * Ravenbrook for commercial licensing options.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 *
 * 3. Redistributions in any form must be accompanied by information on how
 * to obtain complete source code for this software and any accompanying
 * software that uses this software.  The source code must either be
 * included in the distribution or be available for no more than the cost
 * of distribution plus a nominal fee, and must be freely redistributable
 * under reasonable conditions.  For an executable file, complete source
 * code means the source code for all modules it contains. It does not
 * include source code for modules or files that typically accompany the
 * major components of the operating system on which the executable file
 * runs.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE, OR NON-INFRINGEMENT, ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS AND CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
