10 CLS
20 PRINT "Rock, Paper, Scissors Game"
30 PRINT "(1=Rock, 2=Paper, 3=Scissors):"
40 INPUT A
50 REM RANDOMIZE TIMER
60 R = INT(RND(1)*3)+1
70 IF R = 1 THEN C$ = "Rock"
75 IF R = 2 THEN C$ = "Paper"
76 IF R = 3 THEN C$ = "Scissors"
80 IF A = 1 THEN Y$ = "Rock" 
85 IF A = 2 THEN Y$ = "Paper" 
86 IF A = 3 THEN Y$ = "Scissors"
90 PRINT "You: " ; Y$
100 PRINT "Computer: " ; C$
110 IF A = R THEN PRINT "It's a tie!": GOTO 160
120 IF A = 1 AND R = 3 THEN PRINT "You win!": GOTO 160
130 IF A = 2 AND R = 1 THEN PRINT "You win!": GOTO 160
140 IF A = 3 AND R = 2 THEN PRINT "You win!": GOTO 160
150 PRINT "You lose!"
160 GOTO 20