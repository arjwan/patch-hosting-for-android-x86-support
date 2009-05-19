// Copyright 2006 The Android Open Source Project

#ifndef CALL_STACK_H
#define CALL_STACK_H

#include "opcode.h"
#include "armdis.h"

class CallStackBase {
  public:
    int    getId()          { return mId; }
    void   setId(int id)    { mId = id; }

  private:
    int    mId;
};

// Define a template class for the stack frame.  The template parameter
// SYM is the symbol_type from the TraceReader<> template class. To
// use the CallStack class, the user derives a subclass of StackFrame
// and defines push() and pop() methods.  This derived class is then
// passed as a template parameter to CallStack.
template <class SYM>
class StackFrame {
  public:

    virtual ~StackFrame() {};

    virtual void push(int stackLevel, uint64_t time, CallStackBase *base) {};
    virtual void pop(int stackLevel, uint64_t time, CallStackBase *base) {};

    typedef SYM symbol_type;
    static const uint32_t kCausedException = 0x01;
    static const uint32_t kInterpreted     = 0x02;
    static const uint32_t kPopBarrier      = (kCausedException | kInterpreted);

    symbol_type *function;      // the symbol for the function we entered
    uint32_t    addr;           // return address when this function returns
    uint32_t    flags;
    uint32_t    time;           // for debugging when a problem occurred
    uint32_t    global_time;    // for debugging when a problem occurred
};

template <class FRAME, class BASE = CallStackBase>
class CallStack : public BASE {
  public:
    typedef typename FRAME::symbol_type symbol_type;
    typedef typename FRAME::symbol_type::region_type region_type;
    typedef BASE base_type;

    CallStack(int id, int numFrames, TraceReaderType *trace);
    ~CallStack();

    void    updateStack(BBEvent *event, symbol_type *function);
    void    popAll(uint64_t time);
    void    threadStart(uint64_t time);
    void    threadStop(uint64_t time);

    // Set to true if you don't want to see any Java methods
    void    setNativeOnly(bool nativeOnly) {
        mNativeOnly = nativeOnly;
    }

    int         getStackLevel() { return mTop; }

    uint64_t    getGlobalTime(uint64_t time) { return time + mSkippedTime; }
    void        showStack();
    void        showSnapshotStack();

  private:
    enum Action { NONE, PUSH, POP };

    Action      getAction(BBEvent *event, symbol_type *function);
    Action      getMethodAction(BBEvent *event, symbol_type *function);
    void        doSimplePush(symbol_type *function, uint32_t addr,
                             uint64_t time);
    void        doSimplePop(uint64_t time);
    void        doPush(BBEvent *event, symbol_type *function);
    void        doPop(BBEvent *event, symbol_type *function, Action methodAction);

    void        transitionToJava();
    void        transitionFromJava(uint64_t time);

    TraceReaderType *mTrace;
    bool        mNativeOnly;

    symbol_type mDummyFunction;
    region_type mDummyRegion;

    int         mNumFrames;
    FRAME       *mFrames;
    int         mTop;           // index of the next stack frame to write

    int         mJavaTop;

    int         mSnapshotNumFrames;
    FRAME       *mSnapshotFrames;
    int         mSnapshotTop;   // index of the next stack frame to write

    symbol_type *mPrevFunction;
    BBEvent     mPrevEvent;

    symbol_type *mUserFunction;
    BBEvent     mUserEvent;     // the previous user-mode event

    uint64_t    mSkippedTime;
    uint64_t    mLastRunTime;

    static MethodRec    sCurrentMethod;
    static MethodRec    sNextMethod;
};

template<class FRAME, class BASE>
MethodRec CallStack<FRAME, BASE>::sCurrentMethod;
template<class FRAME, class BASE>
MethodRec CallStack<FRAME, BASE>::sNextMethod;

template<class FRAME, class BASE>
CallStack<FRAME, BASE>::CallStack(int id, int numFrames, TraceReaderType *trace)
{
    mNativeOnly = false;
    mTrace = trace;
    BASE::setId(id);
    mNumFrames = numFrames;
    mFrames = new FRAME[mNumFrames];
    mTop = 0;

    mSnapshotNumFrames = numFrames;
    mSnapshotFrames = new FRAME[mSnapshotNumFrames];
    mSnapshotTop = 0;

    memset(&mDummyFunction, 0, sizeof(symbol_type));
    memset(&mDummyRegion, 0, sizeof(region_type));
    mDummyFunction.region = &mDummyRegion;
    mPrevFunction = &mDummyFunction;
    memset(&mPrevEvent, 0, sizeof(BBEvent));
    mUserFunction = &mDummyFunction;
    memset(&mUserEvent, 0, sizeof(BBEvent));
    mSkippedTime = 0;
    mLastRunTime = 0;
    mJavaTop = 0;

    // Read the first two methods from the trace if we haven't already read
    // from the method trace yet.
    if (sCurrentMethod.time == 0) {
        if (mTrace->ReadMethod(&sCurrentMethod)) {
            sCurrentMethod.time = ~0ull;
            sNextMethod.time = ~0ull;
        }
        if (sNextMethod.time != ~0ull && mTrace->ReadMethod(&sNextMethod)) {
            sNextMethod.time = ~0ull;
        }
    }
}

template<class FRAME, class BASE>
CallStack<FRAME, BASE>::~CallStack()
{
    delete mFrames;
}

template<class FRAME, class BASE>
void
CallStack<FRAME, BASE>::updateStack(BBEvent *event, symbol_type *function)
{
    if (mNativeOnly) {
        // If this is an interpreted function, then use the native VM function
        // instead.
        if (function->vm_sym != NULL)
            function = function->vm_sym;
    }

    Action action = getAction(event, function);
    Action methodAction = getMethodAction(event, function);

    // Pop off native functions before pushing or popping Java methods.
    if (action == POP && mPrevFunction->vm_sym == NULL) {
        // Pop off the previous function first.
        doPop(event, function, NONE);
        if (methodAction == POP) {
            doPop(event, function, POP);
        } else if (methodAction == PUSH) {
            doPush(event, function);
        }
    } else {
        if (methodAction != NONE) {
            // If the method trace has a push or pop, then do it.
            action = methodAction;
        } else if (function->vm_sym != NULL) {
            // This function is a Java method.  Don't push or pop the
            // Java method without a corresponding method trace record.
            action = NONE;
        }
        if (action == POP) {
            doPop(event, function, methodAction);
        } else if (action == PUSH) {
            doPush(event, function);
        }
    }

    // If the stack is now empty, then push the current function.
    if (mTop == 0) {
        uint64_t time = event->time - mSkippedTime;
        doSimplePush(function, 0, time);
    }

    mPrevFunction = function;
    mPrevEvent = *event;
}

template<class FRAME, class BASE>
void
CallStack<FRAME, BASE>::threadStart(uint64_t time)
{
    mSkippedTime += time - mLastRunTime;
}

template<class FRAME, class BASE>
void
CallStack<FRAME, BASE>::threadStop(uint64_t time)
{
    mLastRunTime = time;
}

template<class FRAME, class BASE>
typename CallStack<FRAME, BASE>::Action
CallStack<FRAME, BASE>::getAction(BBEvent *event, symbol_type *function)
{
    Action action;
    uint32_t offset;

    // Compute the offset from the start of the function to this basic
    // block address.
    offset = event->bb_addr - function->addr - function->region->base_addr;

    // Get the previously executed instruction
    Opcode op = OP_INVALID;
    int numInsns = mPrevEvent.num_insns;
    uint32_t insn = 0;
    if (numInsns > 0) {
        insn = mPrevEvent.insns[numInsns - 1];
        if (mPrevEvent.is_thumb) {
            insn = insn_unwrap_thumb(insn);
            op = decode_insn_thumb(insn);
        } else {
            op = Arm::decode(insn);
        }
    }

    // The number of bytes in the previous basic block depends on
    // whether the basic block was ARM or THUMB instructions.
    int numBytes;
    if (mPrevEvent.is_thumb) {
        numBytes = numInsns << 1;
    } else {
        numBytes = numInsns << 2;
    }

    // If this basic block follows the previous one, then return NONE.
    // If we don't do this, then we may be fooled into thinking this
    // is a POP if the previous block ended with a conditional
    // (non-executed) ldmia instruction.  We do this check before
    // checking if we are in a different function because we otherwise
    // we might be fooled into thinking this is a PUSH to a new function
    // when it is really just a fall-thru into a local kernel symbol
    // that just looks like a new function.
    uint32_t prev_end_addr = mPrevEvent.bb_addr + numBytes;
    if (prev_end_addr == event->bb_addr) {
        return NONE;
    }

    // If this basic block is in the same function as the last basic block,
    // then just return NONE (but see the exceptions below).
    // Exception 1: if the function calls itself (offset == 0) then we
    // want to push this function.
    // Exception 2: if the function returns to itself, then we want
    // to pop this function.  We detect this case by checking if the last
    // instruction in the previous basic block was a load-multiple (ldm)
    // and included r15 as one of the loaded registers.
    if (function == mPrevFunction) {
        if (numInsns > 0) {
            // If this is the beginning of the function and the previous
            // instruction was not a branch, then it's a PUSH.
            if (offset == 0 && op != OP_B && op != OP_THUMB_B)
                return PUSH;

            // If the previous instruction was an ldm that loaded r15,
            // then it's a POP.
            if (offset != 0 && ((op == OP_LDM && (insn & 0x8000))
                                || (op == OP_THUMB_POP && (insn & 0x100)))) {
                return POP;
            }
        }

        return NONE;
    }

    // We have to figure out if this new function is a call or a
    // return.  We don't necessarily have a complete call stack (since
    // we could have started tracing at any point), so we have to use
    // heuristics.  If the address we are jumping to is the beginning
    // of a function, or if the instruction that took us there was
    // either "bl" or "blx" then this is a PUSH.  Also, if the
    // function offset is non-zero and the previous instruction is a
    // branch instruction, we will call it a PUSH.  This happens in
    // the kernel a lot when there is a branch to an offset from a
    // label. A couple more special cases:
    //
    //   - entering a .plt section ("procedure linkage table") is a PUSH,
    //   - an exception that jumps into the kernel vector entry point
    //     is also a push.
    // 
    // If the function offset is non-zero and the previous instruction
    // is a bx or some non-branch instruction, then it's a POP.
    //
    // There's another special case that comes up.  The user code
    // might execute an instruction that returns but before the pc
    // starts executing in the caller, a kernel interrupt occurs.
    // But it may be hard to tell if this is a return until after
    // the kernel interrupt code is done and returns to user space.
    // So we save the last user basic block and look at it when
    // we come back into user space.

    const uint32_t kIsKernelRegion = region_type::kIsKernelRegion;

    if (((mPrevFunction->region->flags & kIsKernelRegion) == 0)
        && (function->region->flags & kIsKernelRegion)) {
        // We just switched into the kernel.  Save the previous
        // user-mode basic block and function.
        mUserEvent = mPrevEvent;
        mUserFunction = mPrevFunction;
    } else if ((mPrevFunction->region->flags & kIsKernelRegion)
               && ((function->region->flags & kIsKernelRegion) == 0)) {
        // We just switched from kernel to user mode.
        return POP;
    }

    action = PUSH;
    if (offset != 0 && mPrevFunction != &mDummyFunction) {
        // We are jumping into the middle of a function, so this is
        // probably a return, not a function call.  But look at the
        // previous instruction first to see if it was a branch-and-link.

        // If the previous instruction was not a branch (and not a
        // branch-and-link) then POP; or if it is a "bx" instruction
        // then POP because that is used to return from functions.
        if (!isBranch(op) || op == OP_BX || op == OP_THUMB_BX) {
            action = POP;
        } else if (isBranch(op) && !isBranchLink(op)) {
            // If the previous instruction was a normal branch to a
            // local symbol then don't count it as a push or a pop.
            action = NONE;
        }

        if (function->flags & symbol_type::kIsVectorTable)
            action = PUSH;
    }
    return action;
}


template<class FRAME, class BASE>
void CallStack<FRAME, BASE>::doPush(BBEvent *event, symbol_type *function)
{
    uint64_t time = event->time - mSkippedTime;

    // Check for stack overflow
    if (mTop >= mNumFrames) {
#if 0
        showStack();
#endif
        fprintf(stderr, "Error: stack overflow (%d frames)\n", mTop);
        exit(1);
    }

    // Compute the return address here because we may need to change
    // it if we are popping off a frame for a vector table.
    int numBytes;
    if (mPrevEvent.is_thumb) {
        numBytes = mPrevEvent.num_insns << 1;
    } else {
        numBytes = mPrevEvent.num_insns << 2;
    }
    uint32_t retAddr = mPrevEvent.bb_addr + numBytes;

    // If this is a Java method then set the return address to zero.
    // We won't be using it for popping the method and it may lead
    // to false matches when searching the stack.
    if (function->vm_sym != NULL) {
        retAddr = 0;
    }

#if 0
    if (function->flags & symbol_type::kIsVectorStart) {
        printf("stack before entering exception\n");
        showStack();
    }
#endif

    // If the previous function was a vector table, then pop it
    // off before pushing on the new function.  Also, change the
    // return address for the new function to the return address
    // from the vector table.
    if ((mPrevFunction->flags & symbol_type::kIsVectorTable) && mTop > 0) {
        retAddr = mFrames[mTop - 1].addr;
        doSimplePop(time);
    }

    const uint32_t kIsKernelRegion = region_type::kIsKernelRegion;

    // The following code handles the case where one function, F1,
    // calls another function, F2, but the before F2 can start
    // executing, it takes a page fault (on the first instruction
    // in F2).  The kernel is entered, handles the page fault, and
    // then returns to the called function.  The problem is that
    // this looks like a new function call to F2 from the kernel.
    // The following code cleans up the stack by popping the
    // kernel frames back to F1 (but not including F1).  The
    // return address for F2 also has to be fixed up to point to
    // F1 instead of the kernel.
    //
    // We detect this case by checking if the previous basic block
    // was in the kernel and the current basic block is not.
    if ((mPrevFunction->region->flags & kIsKernelRegion)
        && ((function->region->flags & kIsKernelRegion) == 0)
        && mTop > 0) {
        // We are switching from kernel mode to user mode.
#if 0
        printf("  doPush(): popping to user mode, bb_addr: 0x%08x\n",
               event->bb_addr);
        showStack();
#endif
        do {
            // Pop off the kernel frames until we reach the one that
            // caused the exception.
            doSimplePop(time);

            // If the next stack frame is the one that caused an
            // exception then stop popping frames.
            if (mTop > 0
                && (mFrames[mTop - 1].flags & FRAME::kCausedException)) {
                mFrames[mTop - 1].flags &= ~FRAME::kCausedException;
                retAddr = mFrames[mTop].addr;
                break;
            }
        } while (mTop > 0);
#if 0
        printf("  doPush() popping to level %d, using retAddr 0x%08x\n",
               mTop, retAddr);
#endif
    }

    // If we are starting an exception handler, then mark the previous
    // stack frame so that we know where to return when the exception
    // handler finishes.
    if ((function->flags & symbol_type::kIsVectorStart) && mTop > 0)
        mFrames[mTop - 1].flags |= FRAME::kCausedException;

    doSimplePush(function, retAddr, time);
}

template<class FRAME, class BASE>
void CallStack<FRAME, BASE>::doSimplePush(symbol_type *function,
                                          uint32_t addr, uint64_t time)
{
    // Check for stack overflow
    if (mTop >= mNumFrames) {
        showStack();
        fprintf(stderr, "too many stack frames (%d)\n", mTop);
        exit(1);
    }

    // Keep track of the number of Java methods we push on the stack.
    if (!mNativeOnly && function->vm_sym != NULL) {
        // If we are pushing the first Java method on the stack, then
        // save a snapshot of the stack so that we can clean things up
        // later when we pop off the last Java stack frame.
        if (mJavaTop == 0) {
            transitionToJava();
        }
        mJavaTop += 1;
    }

    mFrames[mTop].addr = addr;
    mFrames[mTop].function = function;
    mFrames[mTop].flags = 0;
    mFrames[mTop].time = time;
    mFrames[mTop].global_time = time + mSkippedTime;

    // If the function being pushed is a Java method, then mark it on
    // the stack so that we don't pop it off until we get a matching
    // trace record from the method trace file.
    if (function->vm_sym != NULL) {
        mFrames[mTop].flags = FRAME::kInterpreted;
    }

    mFrames[mTop].push(mTop, time, this);
    mTop += 1;
}

template<class FRAME, class BASE>
void CallStack<FRAME, BASE>::doSimplePop(uint64_t time)
{
    if (mTop <= 0) {
        return;
    }

    mTop -= 1;
    mFrames[mTop].pop(mTop, time, this);

    // Keep track of the number of Java methods we have on the stack.
    symbol_type *function = mFrames[mTop].function;
    if (!mNativeOnly && function->vm_sym != NULL) {
        mJavaTop -= 1;

        // When there are no more Java stack frames, then clean up
        // the client's stack.  We need to do this because the client
        // doesn't see the changes to the native stack underlying the
        // fake Java stack until the last Java method is popped off.
        if (mJavaTop == 0) {
            transitionFromJava(time);
        }
    }
}

template<class FRAME, class BASE>
void CallStack<FRAME, BASE>::doPop(BBEvent *event, symbol_type *function,
                                   Action methodAction)
{
    uint64_t time = event->time - mSkippedTime;

    // Search backward on the stack for a matching return address.
    // The most common case is that we pop one stack frame, but
    // sometimes we pop more than one.
    int stackLevel;
    bool allowMethodPop = (methodAction == POP);
    for (stackLevel = mTop - 1; stackLevel >= 0; --stackLevel) {
        if (event->bb_addr == mFrames[stackLevel].addr) {
            // We found a matching return address on the stack.
            break;
        }

        // If this stack frame caused an exception, then do not pop
        // this stack frame.
        if (mFrames[stackLevel].flags & FRAME::kPopBarrier) {
            // If this is a Java method, then allow a pop only if we
            // have a matching trace record.
            if (mFrames[stackLevel].flags & FRAME::kInterpreted) {
                if (allowMethodPop) {
                    // Allow at most one method pop
                    allowMethodPop = false;
                    continue;
                }
            }
            stackLevel += 1;
            break;
        }
    }

    // If we didn't find a matching return address then search the stack
    // again for a matching function.
    if (stackLevel < 0 || event->bb_addr != mFrames[stackLevel].addr) {
        bool allowMethodPop = (methodAction == POP);
        for (stackLevel = mTop - 1; stackLevel >= 0; --stackLevel) {
            // Compare the function with the one in the stack frame.
            if (function == mFrames[stackLevel].function) {
                // We found a matching function.  We want to pop up to but not
                // including this frame.
                stackLevel += 1;
                break;
            }

            // If this stack frame caused an exception, then do not pop
            // this stack frame.
            if (mFrames[stackLevel].flags & FRAME::kPopBarrier) {
                // If this is a Java method, then allow a pop only if we
                // have a matching trace record.
                if (mFrames[stackLevel].flags & FRAME::kInterpreted) {
                    if (allowMethodPop) {
                        // Allow at most one method pop
                        allowMethodPop = false;
                        continue;
                    }
                }
                stackLevel += 1;
                break;
            }
        }
        if (stackLevel < 0)
            stackLevel = 0;
    }

    // Note that if we didn't find a matching stack frame, we will pop
    // the whole stack (unless there is a Java method or exception
    // frame on the stack).  This is intentional because we may have
    // started the trace in the middle of an executing program that is
    // returning up the stack and we do not know the whole stack.  So
    // the right thing to do is to empty the stack.

    // If we are emptying the stack, then add the current function
    // on top.  If the current function is the same as the top of
    // stack, then avoid an extraneous pop and push.
    if (stackLevel == 0 && mFrames[0].function == function)
        stackLevel = 1;

#if 0
    if (mTop - stackLevel > 7) {
        printf("popping thru level %d\n", stackLevel);
        showStack();
    }
#endif

    // Pop the stack frames
    for (int ii = mTop - 1; ii >= stackLevel; --ii)
        doSimplePop(time);

    // Clear the "caused exception" bit on the current stack frame
    if (mTop > 0) {
        mFrames[mTop - 1].flags &= ~FRAME::kCausedException;
    }

    // Also handle the case where F1 calls F2 and F2 returns to
    // F1, but before we can execute any instructions in F1, we
    // switch to the kernel.  Then when we return from the kernel
    // we want to pop off F2 from the stack instead of pushing F1
    // on top of F2.  To handle this case, we saved the last
    // user-mode basic block when we entered the kernel (in
    // the getAction() function) and now we can check to see if
    // that was a return to F1 instead of a call.  We use the
    // getAction() function to determine this.
    const uint32_t kIsKernelRegion = region_type::kIsKernelRegion;
    if ((mPrevFunction->region->flags & kIsKernelRegion)
        && ((function->region->flags & kIsKernelRegion) == 0)) {
        mPrevEvent = mUserEvent;
        mPrevFunction = mUserFunction;
        if (getAction(event, function) == POP) {
            // We may need to pop more than one frame, so just
            // call doPop() again.  This won't be an infinite loop
            // here because we changed mPrevEvent to the last
            // user-mode event.
            doPop(event, function, methodAction);
            return;
        }
    }
}

template<class FRAME, class BASE>
void CallStack<FRAME, BASE>::popAll(uint64_t time)
{
    time -= mSkippedTime;
    while (mTop != 0) {
        doSimplePop(time);
    }
}

template<class FRAME, class BASE>
typename CallStack<FRAME, BASE>::Action
CallStack<FRAME, BASE>::getMethodAction(BBEvent *event, symbol_type *function)
{
    if (function->vm_sym == NULL && mPrevFunction->vm_sym == NULL) {
        return NONE;
    }

    Action action = NONE;
    uint32_t prevAddr = mPrevFunction->addr + mPrevFunction->region->base_addr;
    uint32_t addr = function->addr + function->region->base_addr;

    // If the events get ahead of the method trace, then read ahead until we
    // sync up again.  This can happen if there is a pop of a method in the
    // method trace for which we don't have a previous push.
    while (event->time >= sNextMethod.time) {
        sCurrentMethod = sNextMethod;
        if (mTrace->ReadMethod(&sNextMethod)) {
            sNextMethod.time = ~0ull;
        }
    }

    if (event->time >= sCurrentMethod.time) {
        if (addr == sCurrentMethod.addr || prevAddr == sCurrentMethod.addr) {
            action = (sCurrentMethod.flags == 0) ? PUSH : POP;
            // We found a match, so read the next record.
            sCurrentMethod = sNextMethod;
            if (sNextMethod.time != ~0ull && mTrace->ReadMethod(&sNextMethod)) {
                sNextMethod.time = ~0ull;
            }
        }
    }
    return action;
}

// When the first Java method is pushed on the stack, this method is
// called to save a snapshot of the current native stack so that the
// client's view of the native stack can be patched up later when the
// Java stack is empty.
template<class FRAME, class BASE>
void CallStack<FRAME, BASE>::transitionToJava()
{
    mSnapshotTop = mTop;
    for (int ii = 0; ii < mTop; ++ii) {
        mSnapshotFrames[ii] = mFrames[ii];
    }
}

// When the Java stack becomes empty, the native stack becomes
// visible.  This method is called when the Java stack becomes empty
// to patch up the client's view of the native stack, which may have
// changed underneath the Java stack.  The stack snapshot is used to
// create a sequence of pops and pushes to make the client's view of
// the native stack match the current native stack.
template<class FRAME, class BASE>
void CallStack<FRAME, BASE>::transitionFromJava(uint64_t time)
{
    int top = mTop;
    if (top > mSnapshotTop) {
        top = mSnapshotTop;
    }
    for (int ii = 0; ii < top; ++ii) {
        if (mSnapshotFrames[ii].function->addr == mFrames[ii].function->addr) {
            continue;
        }

        // Pop off all the rest of the frames from the snapshot
        for (int jj = top - 1; jj >= ii; --jj) {
            mSnapshotFrames[jj].pop(jj, time, this);
        }

        // Push the new frames from the native stack
        for (int jj = ii; jj < mTop; ++jj) {
            mFrames[jj].push(jj, time, this);
        }
        break;
    }
}

template<class FRAME, class BASE>
void CallStack<FRAME, BASE>::showStack()
{
    fprintf(stderr, "mTop: %d skippedTime: %llu\n", mTop, mSkippedTime);
    for (int ii = 0; ii < mTop; ++ii) {
        fprintf(stderr, "  %d: t %d gt %d f %x 0x%08x 0x%08x %s\n",
                ii, mFrames[ii].time, mFrames[ii].global_time,
                mFrames[ii].flags,
                mFrames[ii].addr, mFrames[ii].function->addr,
                mFrames[ii].function->name);
    }
}

template<class FRAME, class BASE>
void CallStack<FRAME, BASE>::showSnapshotStack()
{
    fprintf(stderr, "mSnapshotTop: %d\n", mSnapshotTop);
    for (int ii = 0; ii < mSnapshotTop; ++ii) {
        fprintf(stderr, "  %d: t %d f %x 0x%08x 0x%08x %s\n",
                ii, mSnapshotFrames[ii].time, mSnapshotFrames[ii].flags,
                mSnapshotFrames[ii].addr, mSnapshotFrames[ii].function->addr,
                mSnapshotFrames[ii].function->name);
    }
}

#endif /* CALL_STACK_H */
