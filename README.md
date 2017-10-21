# Neverball Wii

This is a work in progress port of Neverball to the Nintendo Wii. The
game is currently very glitchy and suffers from an abysmal framerate,
but I'm working on improving it. To build the game, install devkitPPC and run `make` inside of this project directory. To run, create an `/apps/neverball` folder on an SD card and copy `boot.dol`, `dist/wii-hbc/meta.xml`, `dist/wii-hbc/icon.png`, and the `data/` directory to the `/apps/neverball` folder on the SD card. It can be launched from the Homebrew Channel on a homebrew-enabled Wii.

# Neverball

Tilt the  floor to roll a  ball through an obstacle  course within the
given  time.  If  the  ball falls  or time  expires, a ball is lost.

Collect coins to unlock the exit  and earn extra balls.  Red coins are
worth 5.  Blue coins are worth 10.  A ball is awarded for 100 coins.

## Release Notes

Release highlights can be found in [doc/release-notes.md](doc/release-notes.md).

## Documentation

* [LICENSE.md](LICENSE.md): a description of licensing and exceptions
* [doc/install.txt](doc/install.txt): instructions on how to build the
  game from source code
* [doc/manual.txt](doc/manual.txt): a detailed description of how to
  play and configure the game
* [doc/authors.txt](doc/authors.txt): a list of people who have
  contributed to Neverball

## Resources

* [Website](http://neverball.org/)
* [Development](http://github.com/Neverball)
* [Neverforum](http://neverforum.com/)
* [Nevertable](http://table.nevercorner.net/) (high-score and replay
  database)
* [#neverball on chat.freenode.net](http://webchat.freenode.net/)

## Translation

Neverball uses the gettext approach to translations. We're always
interested in covering more languages. We have a project on Transifex
(see [instructions on the forum][tx]) and we also accept PO files.

[tx]: http://neverforum.com/fmpbo/viewtopic.php?id=2741
