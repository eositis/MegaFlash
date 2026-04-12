# FLASHSOAK — cc65 ProDOS binary (same load config as cpanel)
CL65      = cl65
CFG       = ../../../cpanel/apple2enh-bin.cfg
OUT       = flashsoak.bin
SRC       = flashsoak.c

.PHONY: all clean

all: $(OUT)

$(OUT): $(SRC) $(CFG)
	$(CL65) -t apple2enh -O -o $@ -C $(CFG) $(SRC)

clean:
	rm -f $(OUT)
