# Flash validation disk — same pattern as ../a2speed/Makefile `disk` target.
# Requires AppleCommander (CLI jar) and Java.

ifneq ($(wildcard /opt/homebrew/opt/openjdk/bin/java),)
JAVA := /opt/homebrew/opt/openjdk/bin/java
else
JAVA := java
endif

# Prefer AppleCommander-ac.jar (same default name as a2speed), then other common installs.
AC_JAR ?= $(firstword $(wildcard $(HOME)/Library/Application\ Support/AppleCommander/AppleCommander-ac.jar) $(wildcard $(HOME)/Library/Application\ Support/AppleCommander/AppleCommander-ac-13.0.jar) $(wildcard $(HOME)/Library/Application\ Support/AppleCommander/ac.jar))

.PHONY: disk

disk:
	@if [ -z "$(AC_JAR)" ]; then \
		echo "Set AC_JAR to AppleCommander ac.jar, e.g.:"; \
		echo "  make disk AC_JAR=/path/to/AppleCommander-ac.jar"; \
		exit 1; \
	fi
	JAVA="$(JAVA)" AC_JAR="$(AC_JAR)" bash ./build-flashval-disk.sh
