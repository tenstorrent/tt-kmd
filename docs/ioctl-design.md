# Ioctl Interface Design

This document describes the extensibility strategy for the tt-kmd ioctl
interface: how to add new ioctls, how to extend existing ones, and the
rationale behind the design choices.

The approach is based on the VFIO pattern from the Linux kernel. For
background on what goes wrong when you get ioctls wrong, read
[Botching up ioctls](https://docs.kernel.org/process/botching-up-ioctls.html),
and for general kernel ioctl guidance see
[ioctl based interfaces](https://docs.kernel.org/driver-api/ioctl.html).


## The Problem

tt-kmd is an out-of-tree DKMS module. Users install it from a .deb or build
it themselves. Userspace tools (tt-smi, tt-topology, UMD, custom programs)
are built and deployed independently. This means:

- A newer userspace tool may run against an older installed kernel module.
- A newer kernel module may be loaded under older userspace tools.

Both directions must work gracefully. A version mismatch should never crash,
corrupt state, or silently ignore new fields that the caller expected to take
effect. The interface must either handle the mismatch transparently or fail
with a clear error.


## The Pattern

Every new ioctl struct starts with `argsz` and `flags`:

```c
struct tenstorrent_example {
    __u32 argsz;        /* sizeof(struct tenstorrent_example) */
    __u32 flags;        /* behavioral flags; unknown bits rejected */
    /* ... fields ... */
};
```

- **`argsz`** answers *"which fields are physically present?"* It is the size,
  in bytes, of the struct the caller was compiled against. The kernel uses it
  to negotiate struct growth at the tail.
- **`flags`** answers *"what behaviour is requested?"* It is a bitmask
  of whole-call modifiers. It is not used to indicate field presence, except
  when a field can't be ignored when 0.

The ioctl command number is defined with `_IO()` (no size encoded):

```c
#define TENSTORRENT_IOCTL_EXAMPLE  _IO(TENSTORRENT_IOCTL_MAGIC, N)
```

This is deliberate. `_IOW`/`_IOR`/`_IOWR` encode struct size into the command
number, which defeats runtime size negotiation via `argsz`.

## Kernel-Side Handling

### Reading input: `copy_struct_from_user()`

The kernel provides `copy_struct_from_user()` which does most of
what we need:

- If the caller's struct is **smaller** than ours, it copies what the caller
  provided and zero-fills the rest of our struct (old caller, new kernel — the
  missing fields default to zero).
- If the caller's struct is **larger** than ours, it copies the portion we
  understand and verifies that the unknown trailing bytes are all zero,
  returning `-E2BIG` if any are nonzero (new caller, old kernel — fail loud
  rather than silently dropping a field the caller set).


### Writing output: lenient, with `argsz` write-back

Input and output have deliberately opposite philosophies, mirroring the kernel
itself: **be strict in what you accept, liberal in what you emit.**

- Input (`copy_struct_from_user`) is **strict**: an unknown nonzero field is
  `-E2BIG`. We refuse to act on input we don't understand.
- Output is **lenient**: if the caller's buffer is too small for a field, that
  field is simply not delivered; if it is larger than we fill, we zero the
  remainder. We never fail a call because of an output-size mismatch.

Transport alone, however, does not tell a *new* caller running on an *old*
kernel whether an appended output field was populated or simply unsupported:
the old kernel zero-fills that region, and zero is ambiguous. To resolve this,
**write `argsz` back as the size the kernel would emit for this call** — not the
number of bytes actually transferred.

Userspace then reasons as:

```c
    valid = min(out.argsz, sizeof(struct mine));   /* bytes I can trust */
    if (valid >= offsetofend(struct mine, new_field))
        use(out.new_field);                        /* this field was written */
    if (out.argsz > sizeof(struct mine))
        ;                                          /* kernel had more to give */
```

The kernel's mirror helper is `copy_struct_to_user()`, but it was not introduced
until v6.13 and buggy until v7.2, so we carry a local copy.

```c
/* copy_struct_to_user() appeared in v6.13; its ignored_trailing logic was
 * fixed later (mainline commit 4911de3145a7). Until our minimum supported
 * kernel includes the fix, use a corrected local copy.
 */
static inline int copy_struct_to_user_fixed(void __user *dst, size_t usize,
                                            const void *src, size_t ksize,
                                            bool *ignored_trailing)
{
    size_t size = min(ksize, usize);
    size_t rest = max(ksize, usize) - size;

    if (usize > ksize) {            /* new caller, old kernel: zero the tail */
        if (clear_user(dst + size, rest))
            return -EFAULT;
    }
    if (ignored_trailing)           /* old caller, new kernel: did we drop data? */
        *ignored_trailing = ksize > usize &&
            memchr_inv(src + size, 0, rest) != NULL;
    if (copy_to_user(dst, src, size))
        return -EFAULT;
    return 0;
}
```

```c

#define READ_IOCTL_INPUT(arg, data, minsz)
  do {
    int read_ioctl_input_ret;

    // Read argsz first so we know how big the caller's struct is.
    if (get_user((data).argsz, &(arg)->argsz))
        return -EFAULT;

    // reject impossibly small inputs
    if ((data).argsz < minsz)
        return -EINVAL;

    // Copy the input, zero-fill missing fields, reject nonzero unknown
    // trailing bytes with -E2BIG.
    read_ioctl_input_ret = copy_struct_from_user(&(data), sizeof(data), (arg), (data).argsz);
    if (read_ioctl_input_ret)
        return read_ioctl_input_ret;

  } while (0)

#define WRITE_IOCTL_OUTPUT(arg, data)
  ({
    uint32_t input_ioctl_size = (data).argsz;
    (data).argsz = sizeof(data);
    copy_struct_to_user_fixed(arg, input_ioctl_size, &data, sizeof(data), NULL);
  })

static long ioctl_example(struct chardev_private *priv,
                          struct tenstorrent_example __user *arg)
{
    /* Minimum size this kernel requires. When the struct is extended this
     * stays pinned to the original size, so old callers keep working. */
    const size_t minsz = offsetofend(struct tenstorrent_example, last_original_field);
    struct tenstorrent_example data = {};

    READ_IOCTL_INPUT(arg, data, minsz);

    /* Reject unknown behavioral flags. */
    if (data.flags & ~TENSTORRENT_EXAMPLE_SUPPORTED_FLAGS)
        return -EINVAL;

    /* Validate reserved fields. */
    if (data.reserved0 != 0)
        return -EINVAL;

    /* ... do the work ... */

    return WRITE_IOCTL_OUTPUT(arg, data);
}
```

## Extending an existing ioctl

To add a field to an existing argsz-style struct:

1. **Append** the new field to the end of the struct in `ioctl.h`. Never
   reorder, remove, or resize existing fields.

2. **Make zero the safe default** for the new field (the guiding principle).
   Choose the encoding so that an old caller's implicit zero, and a new
   caller's explicit zero, both mean "legacy behavior." Do **not** change
   `minsz` in the handler — it stays at the original struct size.

3. **For input fields where zero is a safe default, that is all you need.** The
   field is present exactly when the caller's struct is large enough to contain
   it, and `copy_struct_from_user` has already established that. No flag is
   required:

   ```c
   if (data.argsz >= offsetofend(struct tenstorrent_example, new_field)) {
       /* caller supplied new_field; zero means legacy behavior */
   }
   ```

   (In practice you often don't even need the `argsz` check: if zero is the
   legacy default, just read `data.new_field` — it is zero whether the caller
   omitted it or set it to zero.)

4. **Add a flag bit only when zero cannot be the default** — i.e. when zero is
   itself a meaningful value that must be distinguished from "not specified."
   This is the exception. When you do add such a flag, the flag expresses
   *intent* and `argsz` confirms *presence*; check both, because a caller may
   set a flag bit in uninitialized memory without actually carrying the field:

   ```c
   if (data.flags & TENSTORRENT_EXAMPLE_USE_NEW_FIELD) {
       if (data.argsz < offsetofend(struct tenstorrent_example, new_field))
           return -EINVAL;            /* flag claims a field the struct lacks */
       /* use data.new_field, including a deliberate zero */
   }
   ```

   Add the flag bit to `TENSTORRENT_EXAMPLE_SUPPORTED_FLAGS` so older callers
   that never set it are unaffected and unknown bits are still rejected.

5. **For output fields, no flag is needed** — `argsz` write-back already lets
   the caller detect whether the field was populated (see
   [Writing output](#writing-output-lenient-with-argsz-write-back)).

6. **Update the doc comment** for the struct in `ioctl.h`.


## The userspace side of the contract

This scheme is intentionally fail-loud: there is no "old kernel silently
ignores the new thing" path. A new input field on an old kernel yields
`-E2BIG`; a new flag bit on an old kernel yields `-EINVAL`. Every capability
addition is therefore a hard version gate, and userspace must be prepared for
it. The recipe:

- **Zero-initialize the whole struct**, set `argsz = sizeof(struct)`, and set
  only flag bits you know.
- **Feature-detect** rather than assume. Prefer gating on the driver version
  from `GET_DRIVER_INFO`; alternatively, attempt the call and, on `-EINVAL` /
  `-E2BIG`, fall back (clear the new flag, or retry with a smaller `argsz`).
- **Re-set `argsz` before each call** if you reuse a struct, because the kernel
  overwrites it with the output size on return.


## Rules

These apply to all ioctl structs, not just argsz-style ones:

- **Fixed-width types only.** Use `__u32`, `__u64`, `__s32`, etc. Never
  `int`, `long`, `size_t`, or `enum`.

- **Natural alignment with explicit padding.** Every field must be aligned
  to its size. Insert explicit `reserved` padding fields rather than letting
  the compiler pad. The struct's total size must be a multiple of its largest
  member's size.

- **Validate all reserved fields and padding.** Reject the ioctl if any
  reserved field is nonzero. This preserves the ability to give those
  fields meaning later. If you skip this check, someone will pass stack
  garbage in the reserved field, it will work, and you can never use that
  field for anything else.

- **Zero-initialize kernel-side structs.** Use `= {}`. This prevents leaking
  kernel stack contents to userspace and provides safe defaults for fields not
  present in the caller's struct.

- **Never write past the caller's buffer.** `copy_struct_to_user_fixed` (or
  the upstream helper) enforces this; do not hand-roll a `copy_to_user` that
  ignores the caller's `argsz`.

- **Pointers are `__u64`.** Cast to/from `uintptr_t` in userspace,
  `u64_to_user_ptr()` in kernel. This avoids 32/64-bit compat issues.


## Testing

Mismatch handling is the whole point of the scheme, so test it directly, the
same way the legacy ioctls are tested (verify the driver zeroes extra output
bytes and never reads or writes past the struct size). For each argsz ioctl,
exercise:

- **`argsz` below `minsz`** → `-EINVAL`.
- **`argsz` larger than the kernel's, trailing bytes zero** (new caller, old
  kernel, output-only growth) → success, output truncated cleanly.
- **`argsz` larger than the kernel's, trailing bytes nonzero** (new caller sets
  an unknown input field) → `-E2BIG`.
- **`argsz` smaller than the kernel's** (old caller, new kernel) → success,
  missing fields defaulted to zero, output `argsz` reports the full size.

### Locking the ABI layout

The behavioral tests above prove the kernel *honors* old sizes; a separate
compile-time check proves the structs themselves don't move. `test/pahole_check.sh`
(run in CI by `.github/workflows/check-padding.yml`) compiles every `ioctl.h`
struct with debug info, reads the layout back with `pahole`, and verifies two
things against the locked snapshot in `test/ioctl_abi.golden`:

1. No struct has implicit padding (all padding must be explicit reserved fields).
2. No existing member changes offset or size or disappears, and no struct
   shrinks. Appending a new member to the tail is allowed and reported; any
   other change fails the build.

Because the snapshot pins the offset and size of every member, it pins the
`offsetofend` of each argsz ioctl's `minsz`-defining field — so `minsz` cannot
silently change. A reorder, resize, insertion, or removal anywhere ahead of
that field moves recorded bytes and fails CI.

When you intentionally append a field, regenerate the snapshot and commit it:

```sh
test/pahole_check.sh --update-golden
```

The resulting diff shows only added member lines and the new total size, which
is exactly what a reviewer should expect for legitimate tail growth.


## Legacy Ioctls

The older ioctls in tt-kmd use two patterns that predate the argsz approach.

### The `output_size_bytes` pattern

Used by `GET_DEVICE_INFO`, `GET_DRIVER_INFO`, `RESET_DEVICE`, `PIN_PAGES`.
`QUERY_MAPPINGS` uses a variant where the caller provides an element count
(`output_mapping_count`) instead of a byte size, but the principle is the
same.

```c
struct tenstorrent_get_device_info_in {
    __u32 output_size_bytes;
};
struct tenstorrent_get_device_info_out {
    __u32 output_size_bytes;
    /* ... fields ... */
};
```

The caller says how large its output buffer is. The kernel writes
`min(output_size_bytes, sizeof(out))` bytes back, and sets
`out.output_size_bytes` to the kernel's full output size so the caller can
detect truncation. This is the same "needed size" write-back the argsz scheme
uses for output; the argsz approach simply folds the input and output size
negotiation into a single leading field.

### The fixed-struct pattern

Used by `ALLOCATE_DMA_BUF`, `FREE_DMA_BUF`, `LOCK_CTL`, `MAP_PEER_BAR`,
`ALLOCATE_TLB`, `FREE_TLB`, `CONFIGURE_TLB`, `UNPIN_PAGES`.

No size negotiation. Any struct layout change is an ABI break. Some include
`reserved` fields that can absorb limited future use.

### Extending legacy ioctls

If a legacy ioctl needs new functionality that cannot be expressed through
its existing reserved fields, add a new ioctl number with an argsz-style
struct rather than modifying the existing struct layout. The old ioctl
stays as-is for compatibility.
