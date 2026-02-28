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
    __u32 flags;        /* reserved, must be 0 */
    /* ... fields ... */
};
```

`argsz` is set by the caller to the size of the struct it was compiled against.
The kernel uses this to determine which version of the struct the caller has.

`flags` is reserved for future behavioral variations. The kernel rejects
nonzero values it does not recognize, which prevents callers from accidentally
relying on ignored flags.

The ioctl command number is defined with `_IO()` (no size encoded):

```c
#define TENSTORRENT_IOCTL_EXAMPLE  _IO(TENSTORRENT_IOCTL_MAGIC, N)
```

This is deliberate. `_IOW`/`_IOR`/`_IOWR` encode struct size into the command
number, which defeats the purpose of runtime size negotiation via `argsz`.
VFIO uses the same approach for the same reason.


## Kernel-Side Handling

The kernel handler follows a strict sequence. Here it is in full, annotated:

```c
static long ioctl_example(struct chardev_private *priv,
                          struct tenstorrent_example __user *arg)
{
    /* Minimum size this kernel version requires. When the struct is
     * extended, this stays pinned to the original size so that old
     * callers still work. */
    const size_t minsz = offsetofend(struct tenstorrent_example, last_original_field);

    struct tenstorrent_example data = {};
    size_t copysz;

    /* Step 1: Read argsz from userspace. */
    if (get_user(data.argsz, &arg->argsz))
        return -EFAULT;

    /* Step 2: Reject if caller's struct is smaller than the minimum. */
    if (data.argsz < minsz)
        return -EINVAL;

    /* Step 3: Compute the overlap between caller's struct and ours.
     * This is the number of bytes we can safely exchange. */
    copysz = min_t(size_t, data.argsz, sizeof(data));

    /* Step 4: Copy the overlapping portion. The rest of `data` stays
     * zero-initialized, providing safe defaults for any fields the
     * caller's older struct didn't include. */
    if (copy_from_user(&data, arg, copysz))
        return -EFAULT;

    /* Step 5: Reject unknown trailing bytes that are nonzero.
     * This catches new-userspace-on-old-kernel: if the caller set a
     * field that this kernel doesn't know about, fail loudly rather
     * than silently ignoring it. */
    if (data.argsz > sizeof(data)) {
        if (check_zeroed_user((char __user *)arg + sizeof(data),
                              data.argsz - sizeof(data)) <= 0)
            return -E2BIG;
    }

    /* Step 6: Validate reserved fields. */
    if (data.flags != 0)
        return -EINVAL;

    /* ... do the work ... */

    /* Step 7: Write back only the overlap. Never write past what the
     * caller allocated. */
    if (copy_to_user(arg, &data, copysz))
        return -EFAULT;

    return 0;
}
```

### Why `offsetofend` for `minsz`?

When you add a new field, you do **not** update `minsz`. It remains pinned
to the last field of the *original* struct definition. This is what allows
old userspace to keep working: their smaller `argsz` is still `>= minsz`,
so they pass the check. The new fields are zero in `data` (from `= {}`),
and the kernel treats zero as "not specified" and applies a default.

### Why `check_zeroed_user`?

Without this check, new userspace that sets a newly added field would get
silent success from an old kernel that doesn't know about that field. The
caller thinks it requested something; the kernel ignored it. This is
especially dangerous for a DKMS driver where mismatched versions are common.

With the check, the caller gets `-E2BIG`, which is a clear signal: "you
passed fields I don't understand." The caller can then fall back or report
a version mismatch.

This is stricter than what VFIO does (VFIO silently ignores the tail).
The kernel's `copy_struct_from_user()` helper uses the same strict approach.
For an out-of-tree driver where version skew is routine, strict is better.


## Extending an Existing Ioctl

To add a field to an existing argsz-style struct:

1. **Append** the new field to the end of the struct in `ioctl.h`. Never
   reorder, remove, or resize existing fields.

2. **Add a flag bit** if the new field changes behavior. New flag bits are
   defined in `ioctl.h` alongside the struct. The kernel checks the flag
   before reading the new field.

3. **Do not change `minsz`** in the handler. It stays at the original struct
   size. The handler checks whether the new field was provided by comparing
   `copysz` against `offsetofend(struct ..., new_field)`:

   ```c
   /* New field is present if the caller's struct is large enough. */
   if (copysz >= offsetofend(struct tenstorrent_example, new_field)) {
       /* use data.new_field */
   } else {
       /* default behavior, data.new_field is zero from = {} */
   }
   ```

4. **Zero must be a safe default** for the new field. Old callers won't set
   it, so the handler will see zero.

5. **Update the doc comment** for the struct in `ioctl.h`.


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

- **Zero-initialize kernel-side structs.** Use `= {}` or `= {0}`. This
  prevents leaking kernel stack contents to userspace (information leak)
  and provides safe defaults for fields not present in the caller's struct.

- **Never write past `argsz`.** Always compute
  `min_t(size_t, argsz, sizeof(data))` and use that as the copy-to-user
  size.

- **Pointers are `__u64`.** Cast to/from `uintptr_t` in userspace,
  `u64_to_user_ptr()` in kernel. This avoids 32/64-bit compat issues.


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
detect truncation.

This provides forward compatibility for outputs. The input struct has no
size negotiation.

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
