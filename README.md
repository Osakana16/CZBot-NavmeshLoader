# CS1.6 and CZero NavArea Loader
This repository provides some refactored [source code from HLSDK](https://github.com/ValveSoftware/halflife/blob/master/game_shared/bot/nav_file.cpp), for loading CZBot's navigation mesh.
By using this code, you can get the navmesh data from a nav file and use it for your plugins easily.

The classes SpotOrder, SpotEncounter, NavLadder,NavAreaGrid, PlaceDirectory, and NavArea are provided.

This repository is also a Metamod plugin; You can check the code works by using command to get the navmeh ID.

# Requires to compile
* [HLSDK](https://github.com/ValveSoftware/halflife/tree/master)
* [Metamod](http://metamod.org/)

# Commands
* loadnav - Load the nav file of the current map in cstrike or czero.
* getnav - Get the navmesh ID from your position.