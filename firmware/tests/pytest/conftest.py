"""Shared pytest-embedded fixtures for clawlexa device tests.

These tests assume `idf.py build` has been run in firmware/ first. We do not
re-build inside the test session to keep iteration loops tight; CI / make
targets should do the build explicitly.
"""
