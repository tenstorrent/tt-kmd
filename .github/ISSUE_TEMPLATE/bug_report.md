---
name: Bug Report
about: Report a bug, unexpected behavior, or crash in the Linux Kernel driver.
title: "[Bug]: "
labels: ["bug"]
assignees: ''
---

### Describe the Bug

A clear and concise description of what the bug is. What happened that you didn't expect?

### Steps to Reproduce

Please provide clear, step-by-step instructions to reproduce the behavior. This is crucial for us to verify and fix the bug.

1.  Step 1: ...
2.  Step 2: ...
3.  Step 3: ...
...

### Expected Behavior

What did you expect to happen?

### Actual Behavior

What actually happened? Include any error messages, crashes, or unexpected outputs.

### Kernel Information

Please provide the following details about your Linux kernel environment:

* **Linux Distribution & Version:** (e.g., Ubuntu 22.04 LTS, Fedora 39, Debian 12)
* **Kernel Version:** (Output of `uname -a`):
* **Kernel Configuration:** (e.g., custom build, standard distro kernel)
* **Boot Parameters (if custom):**

### Driver Information

* **KMD Version/Commit:** (e.g., v1.2.3, git commit hash `abcdef123`, or `main` branch from `DATE`)
* **How was KMD built/installed?** (e.g., `make && sudo make install`, DKMS, part of a distro kernel package)

### Hardware Information

* **Specific Device Affected:** (e.g., p150a, n300, etc.)
* **Host System Model:** (e.g., Dell XPS 15, Custom PC)
* **CPU:**

### Relevant Logs and Output

Please include any relevant logs. Use Markdown code blocks for readability.

* **`dmesg` output:**
    ```bash
    # Paste relevant dmesg output here, especially from the time of the bug.
    # Consider piping to a file: dmesg > dmesg_bug.log
    ```
* **Kernel Panic/Oops Log (if applicable):**
    ```
    # Paste kernel panic/oops messages here.
    ```
* **Other relevant log files:** (e.g., `journalctl` output filtered by time or service)
    ```bash
    # Example: journalctl -u systemd --since "yesterday"
    ```
* **Output of any commands you ran:**
    ```bash
    # Example: Output of `lsmod | grep your_driver`
    ```

### Screenshots or Videos (Optional)

If applicable, add screenshots or videos to help explain your problem.

### Additional Context

Add any other context about the problem here (e.g., did it work in a previous version? what are your temporary workarounds?).