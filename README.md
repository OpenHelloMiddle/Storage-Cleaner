# Mouse-Mover
The Mouse-Moverm project is a component of OpenHelloMiddle, which can be used to move the mouse pointer.

Usage: 

"Options:

-x=[value]    Move mouse on X axis. Value can be:

+ : Move right 1 pixel

- : Move left 1 pixel

+N : Move right N pixels

-N : Move left N pixels

N : Move to absolute position N

(empty) : Don't move on X axis

-y=[value]    Move mouse on Y axis. Same values as -x

-h, --help    Show this help message

Examples:

-x=+ -y=+        # Move right and down 1 pixel

-x=+100 -y=-50   # Move right 100 pixels, up 50 pixels

-x=100 -y=200    # Move to absolute position (100,200)

-x=- -y=+        # Move left 1 pixel, down 1 pixel

-x= -y=100       # X doesn't move, Y moves to 100
