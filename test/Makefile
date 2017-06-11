CC = arm-linux-gnueabihf-gcc
TARGET = g2d-opengl-video-render
OBJ = g2d-opengl-video-render.o
CFLAGS ?= -Wall -O2
INC = \
	 -L $(WORKSPACE)/allwinner/buildroot-2016.08.1/A20/staging/usr/lib \
	 -I $(WORKSPACE)/allwinner/buildroot-2016.08.1/A20/staging/usr/include \
	 -I $(WORKSPACE)/allwinner/kernel/linux-sunxi-Ax/include
all: $(TARGET)

$(TARGET): $(OBJ)
	${CC} -o $(TARGET) $(OBJ) $(INC) ${CFLAGS} -lMali -lUMP -lmpv -lm -ldl
%.o: %.c
	${CC} $(INC) ${CFLAGS} -c $^ -o $@
clean:
	rm -f $(TARGET) $(OBJ)
