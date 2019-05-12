# sortingVisualizer (Work In Progress)
#TODO:  
-add more sorting algorithms  
-make it into a command line tool (eg sortingVisualizer -f fileName -s bubble)  
-clean up the code, move around the video class  

A program to create .mp4 videos of sorting algorithms working on given images

This is done using stb_image to import an image, and ffmpeg to encode the final video.
This program works best with small images, as it sorts by position, so the size of the array being sorted is the length by width

Thanks to Dmytro Dovzhenko for the VideoCapture class

